import CoreBluetooth
import Foundation
import Observation
import VoidHealthCore

/// Keeps a connection to the watch and pulls its history outbox.
///
/// On every connection: subscribe to DATA, read INFO, ask for everything
/// after the last record this phone holds (SEND), save each batch to disk,
/// then ACK the highest saved seq so the watch can erase it. The watch keeps
/// streaming new records while connected. iOS keeps a pending connect alive
/// in the background, so the app syncs whenever the watch is in range.
/// Protocol: companion/PROTOCOL.md.
@MainActor
@Observable
final class WatchLink: NSObject {
    enum Status: Equatable {
        case bluetoothOff, unauthorized, unsupported, searching, waiting, connecting, syncing, upToDate, needsFirmware

        var text: String {
            switch self {
            case .bluetoothOff: return "Bluetooth is off"
            case .unauthorized: return "Bluetooth permission needed"
            case .unsupported: return "This device has no Bluetooth LE"
            case .searching: return "Looking for your watch"
            case .waiting: return "Waiting for your watch"
            case .connecting: return "Connecting"
            case .syncing: return "Syncing"
            case .upToDate: return "Up to date"
            case .needsFirmware: return "Watch needs VoidOS 2.3 or newer"
            }
        }

        var isProblem: Bool {
            switch self {
            case .bluetoothOff, .unauthorized, .unsupported, .needsFirmware: return true
            default: return false
            }
        }
    }

    private(set) var status: Status = .searching
    private(set) var watchName: String?
    private(set) var firmware: String?
    private(set) var battery: Int?
    private(set) var liveBPM: Int?
    private(set) var liveSteps: Int?
    private(set) var liveStepsAt: Date?
    private(set) var info: HistoryInfo?
    private(set) var receivedThisSession = 0
    private(set) var lastError: String?
    private(set) var lastSync: Date?
    private(set) var hasWatch = false
    var isConnected: Bool { status == .syncing || status == .upToDate || status == .needsFirmware }

    @ObservationIgnored var onNewRecords: (() -> Void)?
    @ObservationIgnored var onLiveSteps: (() -> Void)?
    @ObservationIgnored private let store: RecordStore
    @ObservationIgnored private var central: CBCentralManager!
    @ObservationIgnored private var peripheral: CBPeripheral?
    @ObservationIgnored private var infoChr: CBCharacteristic?
    @ObservationIgnored private var dataChr: CBCharacteristic?
    @ObservationIgnored private var ctrlChr: CBCharacteristic?
    @ObservationIgnored private var dataSubscribed = false
    @ObservationIgnored private var sentSend = false
    @ObservationIgnored private var cursor = SyncCursor()
    @ObservationIgnored private var ackTask: Task<Void, Never>?
    @ObservationIgnored private var encRetries = 0
    @ObservationIgnored private var encRetryTask: Task<Void, Never>?
    /// the daily goals for the watch to celebrate (VoidOS 2.4.0+)
    @ObservationIgnored private var goals: [Int]?
    @ObservationIgnored private var goalsSent: [Int]?
    /// ops of the CTRL writes waiting for their response, oldest first
    @ObservationIgnored private var ctrlOps: [UInt8] = []

    private static let idKey = "watchID"
    private static let syncKey = "lastSync"
    @ObservationIgnored private let histService = CBUUID(string: WatchUUID.historyService)
    @ObservationIgnored private let histInfo = CBUUID(string: WatchUUID.historyInfo)
    @ObservationIgnored private let histData = CBUUID(string: WatchUUID.historyData)
    @ObservationIgnored private let histCtrl = CBUUID(string: WatchUUID.historyControl)
    @ObservationIgnored private let hrService = CBUUID(string: WatchUUID.heartRateService)
    @ObservationIgnored private let hrMeasure = CBUUID(string: WatchUUID.heartRateMeasurement)
    @ObservationIgnored private let battService = CBUUID(string: WatchUUID.batteryService)
    @ObservationIgnored private let battLevel = CBUUID(string: WatchUUID.batteryLevel)
    @ObservationIgnored private let timeService = CBUUID(string: WatchUUID.timeService)
    @ObservationIgnored private let currentTime = CBUUID(string: WatchUUID.currentTime)
    @ObservationIgnored private let disService = CBUUID(string: WatchUUID.deviceInfoService)
    @ObservationIgnored private let fwRevision = CBUUID(string: WatchUUID.firmwareRevision)
    @ObservationIgnored private let motionService = CBUUID(string: WatchUUID.motionService)
    @ObservationIgnored private let stepCount = CBUUID(string: WatchUUID.stepCount)

    init(store: RecordStore) {
        self.store = store
        super.init()
        lastSync = UserDefaults.standard.object(forKey: Self.syncKey) as? Date
        hasWatch = UserDefaults.standard.string(forKey: Self.idKey) != nil
        central = CBCentralManager(delegate: self, queue: .main, options: [
            CBCentralManagerOptionRestoreIdentifierKey: "VoidHealthCentral",
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    // MARK: actions

    /// app came to the foreground or the user pulled to refresh
    func refresh() {
        guard central.state == .poweredOn else { return }
        guard let p = peripheral, p.state == .connected, infoChr != nil else {
            reconnect()
            return
        }
        sentSend = false                 // the INFO reply starts a new SEND
        if let c = infoChr { p.readValue(for: c) }
        if let c = battChr { p.readValue(for: c) }
    }

    /// drop the saved watch and look for one again
    func forgetWatch() {
        UserDefaults.standard.removeObject(forKey: Self.idKey)
        hasWatch = false
        let old = peripheral
        peripheral = nil
        if let old { central.cancelPeripheralConnection(old) }
        watchName = nil
        firmware = nil
        battery = nil
        info = nil
        liveBPM = nil
        reset()
        reconnect()
    }

    @ObservationIgnored private var battChr: CBCharacteristic?

    private func reconnect() {
        guard central.state == .poweredOn else { return }
        if let p = peripheral {
            switch p.state {
            case .connected:
                if infoChr == nil && dataChr == nil { didConnect(p) }   // restored by iOS, not set up yet
                return
            case .connecting:
                return
            default:
                connect(p)
                return
            }
        }
        if let s = UserDefaults.standard.string(forKey: Self.idKey), let id = UUID(uuidString: s),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(p)
            return
        }
        // the watch is usually already connected to the iPhone for notifications
        let linked = central.retrieveConnectedPeripherals(withServices: [histService])
            + central.retrieveConnectedPeripherals(withServices: [hrService]).filter { Self.looksLikeWatch($0.name) }
        if let p = linked.first {
            connect(p)
            return
        }
        status = .searching
        if !central.isScanning {
            // VoidOS advertises its name and the ANCS solicitation, not a
            // service list, so scan for everything and match the name
            central.scanForPeripherals(withServices: nil, options: nil)
        }
    }

    private static func looksLikeWatch(_ name: String?) -> Bool {
        guard let n = name?.lowercased() else { return false }
        return WatchUUID.names.contains { n.contains($0.lowercased()) }
    }

    private func connect(_ p: CBPeripheral) {
        if central.isScanning { central.stopScan() }
        peripheral = p
        p.delegate = self
        watchName = p.name ?? watchName
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Self.idKey)
        hasWatch = true
        if p.state == .connected {
            didConnect(p)
        } else {
            status = .connecting
            central.connect(p, options: nil)      // stays pending until the watch is in range
        }
    }

    private func reset() {
        infoChr = nil
        dataChr = nil
        ctrlChr = nil
        battChr = nil
        dataSubscribed = false
        sentSend = false
        ackTask?.cancel()
        ackTask = nil
        encRetryTask?.cancel()
        encRetryTask = nil
        encRetries = 0
        goalsSent = nil
        ctrlOps = []
    }

    private func didConnect(_ p: CBPeripheral) {
        reset()
        status = .connecting
        receivedThisSession = 0
        watchName = p.name ?? watchName
        p.discoverServices([histService, hrService, battService, timeService, disService, motionService])
    }

    // MARK: history sync

    private func startSyncIfReady() {
        guard let p = peripheral, let info, let ctrl = ctrlChr, dataSubscribed, !sentSend else { return }
        sentSend = true
        let from = cursor.start(info: info, phoneLast: store.last(log: info.logID))
        writeCtrl(HistoryControl.send(from: from), p, ctrl)
        sendAck()                         // saved last time, ack lost on the way
        sendGoalsIfReady()
        if cursor.upToDate {
            markSynced()
        } else {
            status = .syncing
        }
    }

    private func handle(batch: [UInt8]) {
        guard let info else { return }
        let recs = WatchRecord.decodeBatch(batch)
        guard !recs.isEmpty else { return }
        do {
            let fresh = try store.add(recs, log: info.logID)
            receivedThisSession += fresh.count
            cursor.saved(recs.map(\.seq))
            if !fresh.isEmpty { onNewRecords?() }
            if cursor.ackIsDue {
                sendAck()
            } else {
                ackTask?.cancel()
                ackTask = Task { [weak self] in
                    try? await Task.sleep(for: .seconds(1))
                    guard !Task.isCancelled else { return }
                    self?.sendAck()
                }
            }
            if cursor.upToDate {
                markSynced()
            } else {
                status = .syncing
            }
            lastError = nil
        } catch {
            // not saved, so not acknowledged: the watch keeps these records
            lastError = "Could not save records: \(error.localizedDescription)"
        }
    }

    private func markSynced() {
        status = .upToDate
        let now = Date()
        lastSync = now
        UserDefaults.standard.set(now, forKey: Self.syncKey)
    }

    private func sendAck() {
        guard let p = peripheral, p.state == .connected, let ctrl = ctrlChr,
              let seq = cursor.takeAck() else { return }
        writeCtrl(HistoryControl.ack(through: seq), p, ctrl)
        if let c = infoChr {
            // re-read so the watch status shows what is still waiting
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(2))
                guard let self, let p = self.peripheral, p.state == .connected else { return }
                p.readValue(for: c)
            }
        }
    }

    private func writeCtrl(_ cmd: [UInt8], _ p: CBPeripheral, _ ctrl: CBCharacteristic) {
        ctrlOps.append(cmd[0])
        p.writeValue(Data(cmd), for: ctrl, type: .withResponse)
    }

    // MARK: goals

    /// Tell the watch the daily goals it celebrates. Sent once per
    /// connection, and again whenever they change.
    func setGoals(steps: Int, waterML: Int) {
        goals = [steps, waterML]
        sendGoalsIfReady()
    }

    private func sendGoalsIfReady() {
        guard let goals, goals != goalsSent, let p = peripheral, p.state == .connected,
              let ctrl = ctrlChr, sentSend else { return }
        goalsSent = goals
        writeCtrl(HistoryControl.goals(steps: goals[0], waterML: goals[1]), p, ctrl)
    }

    // MARK: delegate handlers (always on the main queue)

    fileprivate func handleState(_ state: CBManagerState) {
        switch state {
        case .poweredOn: reconnect()
        case .unauthorized: status = .unauthorized
        case .unsupported: status = .unsupported
        default:
            status = .bluetoothOff
            reset()
        }
    }

    fileprivate func handleRestore(_ peripherals: [CBPeripheral]) {
        let saved = UserDefaults.standard.string(forKey: Self.idKey)
        if let p = peripherals.first(where: { $0.identifier.uuidString == saved }) ?? peripherals.first {
            peripheral = p
            p.delegate = self
            watchName = p.name ?? watchName
        }
    }

    fileprivate func handleDiscover(_ p: CBPeripheral, name: String?) {
        guard peripheral == nil, Self.looksLikeWatch(name ?? p.name) else { return }
        connect(p)
    }

    fileprivate func handleConnected(_ p: CBPeripheral) {
        guard p == peripheral else { return }
        didConnect(p)
    }

    fileprivate func handleDisconnected(_ p: CBPeripheral) {
        guard p == peripheral else { return }
        reset()
        liveBPM = nil
        status = .waiting
        if central.state == .poweredOn {
            central.connect(p, options: nil)      // reconnect when back in range
        }
    }

    fileprivate func handleServices(_ p: CBPeripheral, error: Error?) {
        if let error {
            lastError = "Could not read the watch's services: \(error.localizedDescription)"
            return
        }
        let services = p.services ?? []
        if !services.contains(where: { $0.uuid == histService }) { status = .needsFirmware }
        for s in services {
            switch s.uuid {
            case histService: p.discoverCharacteristics([histInfo, histData, histCtrl], for: s)
            case hrService: p.discoverCharacteristics([hrMeasure], for: s)
            case battService: p.discoverCharacteristics([battLevel], for: s)
            case timeService: p.discoverCharacteristics([currentTime], for: s)
            case disService: p.discoverCharacteristics([fwRevision], for: s)
            case motionService: p.discoverCharacteristics([stepCount], for: s)
            default: break
            }
        }
    }

    fileprivate func handleCharacteristics(_ p: CBPeripheral, _ s: CBService) {
        for c in s.characteristics ?? [] {
            switch c.uuid {
            case histInfo:
                infoChr = c
                p.readValue(for: c)                // encrypted: uses the iPhone's bond with the watch
            case histData:
                dataChr = c
                p.setNotifyValue(true, for: c)
            case histCtrl:
                ctrlChr = c
            case hrMeasure:
                p.setNotifyValue(true, for: c)
            case battLevel, stepCount:
                if c.uuid == battLevel { battChr = c }
                p.readValue(for: c)
                if c.properties.contains(.notify) { p.setNotifyValue(true, for: c) }
            case fwRevision:
                p.readValue(for: c)
            case currentTime:
                // keep the watch clock right; the watch only takes it from a bonded phone
                if c.properties.contains(.write) {
                    p.writeValue(Data(WatchValues.currentTime(Date())), for: c, type: .withResponse)
                }
            default: break
            }
        }
    }

    /// The first encrypted read on a new connection fails while iOS pairs
    /// (or re-encrypts) with the watch. iOS starts that pairing itself; we
    /// just try again once it has had time to finish.
    private func isEncryptionError(_ error: Error) -> Bool {
        guard let e = error as? CBATTError else { return false }
        return e.code == .insufficientEncryption || e.code == .insufficientAuthentication
            || e.code == .insufficientEncryptionKeySize
    }

    private func retryAfterPairing(_ c: CBCharacteristic) -> Bool {
        guard encRetries < 10, let p = peripheral, p.state == .connected else { return false }
        encRetries += 1
        encRetryTask?.cancel()
        encRetryTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard let self, !Task.isCancelled, let p = self.peripheral, p.state == .connected else { return }
            if let d = self.dataChr, !self.dataSubscribed { p.setNotifyValue(true, for: d) }
            if let i = self.infoChr { self.sentSend = false; p.readValue(for: i) }
        }
        return true
    }

    fileprivate func handleNotifyState(_ c: CBCharacteristic, error: Error?) {
        guard c.uuid == histData else { return }
        if let error {
            if isEncryptionError(error), retryAfterPairing(c) { return }
            lastError = "History subscription failed: \(error.localizedDescription)"
            return
        }
        dataSubscribed = c.isNotifying
        startSyncIfReady()
    }

    fileprivate func handleValue(_ c: CBCharacteristic, error: Error?) {
        if let error {
            if c.uuid == histInfo {
                if isEncryptionError(error), retryAfterPairing(c) { return }
                lastError = "Could not read the watch history: \(error.localizedDescription)"
            }
            return
        }
        if c.uuid == histInfo { encRetries = 0; lastError = nil }
        guard let v = c.value else { return }
        let bytes = [UInt8](v)
        switch c.uuid {
        case histInfo:
            guard let i = HistoryInfo(bytes) else {
                status = .needsFirmware
                return
            }
            info = i
            if sentSend {
                cursor.update(i)
            } else {
                startSyncIfReady()
            }
        case histData:
            handle(batch: bytes)
        case hrMeasure:
            if let b = WatchValues.heartRate(bytes) { liveBPM = b > 0 ? b : nil }
        case battLevel:
            battery = bytes.first.map(Int.init)
        case fwRevision:
            firmware = String(decoding: bytes, as: UTF8.self)
        case stepCount:
            if let s = WatchValues.steps(bytes) {
                liveSteps = s
                liveStepsAt = Date()
                onLiveSteps?()
            }
        default: break
        }
    }

    fileprivate func handleWrite(_ c: CBCharacteristic, error: Error?) {
        guard c.uuid == histCtrl else { return }
        let op = ctrlOps.isEmpty ? nil : ctrlOps.removeFirst()
        guard let error else { return }
        if op == HistoryControl.goalsOp {
            // firmware before 2.4.0 doesn't know the goals: not a sync
            // problem. Try again on the next sync.
            goalsSent = nil
            return
        }
        lastError = "The watch refused a sync command: \(error.localizedDescription)"
        sentSend = false
    }
}

// CoreBluetooth calls these on the main queue (the manager was made with
// queue: .main), so hopping onto the main actor is safe.
extension WatchLink: CBCentralManagerDelegate, CBPeripheralDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let s = central.state
        MainActor.assumeIsolated { handleState(s) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        let ps = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] ?? []
        MainActor.assumeIsolated { handleRestore(ps) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        MainActor.assumeIsolated { handleDiscover(peripheral, name: name) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { handleConnected(peripheral) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated { handleDisconnected(peripheral) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated { handleDisconnected(peripheral) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated { handleServices(peripheral, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        MainActor.assumeIsolated { handleCharacteristics(peripheral, service) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated { handleNotifyState(characteristic, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated { handleValue(characteristic, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated { handleWrite(characteristic, error: error) }
    }
}
