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
@MainActor
@Observable
final class WatchLink: NSObject {
    enum Status: Equatable {
        case bluetoothOff, unauthorized, searching, connecting, syncing, upToDate, needsFirmware

        var text: String {
            switch self {
            case .bluetoothOff: return "Bluetooth is off"
            case .unauthorized: return "Bluetooth permission needed"
            case .searching: return "Looking for your watch"
            case .connecting: return "Connecting"
            case .syncing: return "Syncing"
            case .upToDate: return "Up to date"
            case .needsFirmware: return "Watch needs VoidOS 2.3 or newer"
            }
        }
    }

    private(set) var status: Status = .searching
    private(set) var watchName: String?
    private(set) var battery: Int?
    private(set) var liveBPM: Int?
    private(set) var liveSteps: Int?
    private(set) var liveStepsAt: Date?
    private(set) var info: HistoryInfo?
    private(set) var receivedThisSession = 0
    private(set) var lastError: String?
    private(set) var lastSync: Date? {
        didSet { UserDefaults.standard.set(lastSync, forKey: "lastSync") }
    }
    var isConnected: Bool { peripheral?.state == .connected }
    var hasWatch: Bool { UserDefaults.standard.string(forKey: Self.idKey) != nil }

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
    @ObservationIgnored private var lastAckSent: UInt32 = 0
    @ObservationIgnored private var highestSaved: UInt32 = 0
    @ObservationIgnored private var ackTask: Task<Void, Never>?

    private static let idKey = "watchID"
    private let histService = CBUUID(string: WatchUUID.historyService)
    private let histInfo = CBUUID(string: WatchUUID.historyInfo)
    private let histData = CBUUID(string: WatchUUID.historyData)
    private let histCtrl = CBUUID(string: WatchUUID.historyControl)
    private let hrService = CBUUID(string: WatchUUID.heartRateService)
    private let hrMeasure = CBUUID(string: WatchUUID.heartRateMeasurement)
    private let battService = CBUUID(string: WatchUUID.batteryService)
    private let battLevel = CBUUID(string: WatchUUID.batteryLevel)
    private let timeService = CBUUID(string: WatchUUID.timeService)
    private let currentTime = CBUUID(string: WatchUUID.currentTime)
    private let motionService = CBUUID(string: WatchUUID.motionService)
    private let stepCount = CBUUID(string: WatchUUID.stepCount)

    init(store: RecordStore) {
        self.store = store
        super.init()
        lastSync = UserDefaults.standard.object(forKey: "lastSync") as? Date
        central = CBCentralManager(delegate: self, queue: .main, options: [
            CBCentralManagerOptionRestoreIdentifierKey: "VoidHealthCentral",
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    // MARK: actions

    /// app came to the foreground: refresh and pull anything new
    func refresh() {
        guard central.state == .poweredOn else { return }
        guard let p = peripheral, p.state == .connected else {
            reconnect()
            return
        }
        if let c = infoChr { p.readValue(for: c) }
        sentSend = false
    }

    /// drop the saved watch and look for one again
    func forgetWatch() {
        UserDefaults.standard.removeObject(forKey: Self.idKey)
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        watchName = nil
        info = nil
        reset()
        reconnect()
    }

    private func reconnect() {
        guard central.state == .poweredOn else { return }
        if let p = peripheral, p.state == .connected || p.state == .connecting { return }
        if let s = UserDefaults.standard.string(forKey: Self.idKey), let id = UUID(uuidString: s),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(p)
            return
        }
        // the watch is often already connected to the iPhone (notifications)
        let linked = central.retrieveConnectedPeripherals(withServices: [histService])
            + central.retrieveConnectedPeripherals(withServices: [hrService]).filter { Self.looksLikeWatch($0.name) }
        if let p = linked.first {
            connect(p)
            return
        }
        status = .searching
        if !central.isScanning {
            central.scanForPeripherals(withServices: [hrService], options: nil)
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
        dataSubscribed = false
        sentSend = false
        ackTask?.cancel()
    }

    private func didConnect(_ p: CBPeripheral) {
        reset()
        status = .connecting
        watchName = p.name ?? watchName
        p.discoverServices([histService, hrService, battService, timeService, motionService])
    }

    // MARK: history sync

    private func startSyncIfReady() {
        guard let p = peripheral, let info, let ctrl = ctrlChr, dataSubscribed, !sentSend else { return }
        sentSend = true
        let held = store.last(log: info.logID)
        let from = HistoryControl.resumeSeq(phoneLast: held, info: info)
        lastAckSent = info.ackedSeq
        highestSaved = from > 0 ? from - 1 : 0
        p.writeValue(Data(HistoryControl.send(from: from)), for: ctrl, type: .withResponse)
        // saved here before but the ack never reached the watch
        if let held, held > info.ackedSeq, held < info.nextSeq {
            highestSaved = max(highestSaved, held)
            sendAck()
        }
        if from >= info.nextSeq {
            status = .upToDate
            lastSync = Date()
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
            if let top = recs.map(\.seq).max(), top > highestSaved { highestSaved = top }
            if highestSaved + 1 > info.nextSeq { self.info?.nextSeq = highestSaved + 1 }
            if !fresh.isEmpty { onNewRecords?() }
            if highestSaved - lastAckSent >= 255 {
                sendAck()
            } else {
                ackTask?.cancel()
                ackTask = Task { [weak self] in
                    try? await Task.sleep(for: .seconds(1))
                    guard !Task.isCancelled else { return }
                    self?.sendAck()
                }
            }
            if highestSaved + 1 >= (self.info?.nextSeq ?? 0) {
                status = .upToDate
                lastSync = Date()
            }
            lastError = nil
        } catch {
            // not saved, so not acknowledged: the watch keeps these records
            lastError = "Could not save records: \(error.localizedDescription)"
        }
    }

    private func sendAck() {
        guard let p = peripheral, let ctrl = ctrlChr, highestSaved > lastAckSent else { return }
        p.writeValue(Data(HistoryControl.ack(through: highestSaved)), for: ctrl, type: .withResponse)
        lastAckSent = highestSaved
        if let c = infoChr {
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(2))
                self?.peripheral?.readValue(for: c)
            }
        }
    }

    // MARK: delegate handlers (always on the main queue)

    fileprivate func handleState(_ state: CBManagerState) {
        switch state {
        case .poweredOn: reconnect()
        case .unauthorized: status = .unauthorized
        default: status = .bluetoothOff
        }
    }

    fileprivate func handleRestore(_ peripherals: [CBPeripheral]) {
        if let p = peripherals.first {
            peripheral = p
            p.delegate = self
        }
    }

    fileprivate func handleDiscover(_ p: CBPeripheral, name: String?) {
        guard Self.looksLikeWatch(name ?? p.name) else { return }
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
        status = .connecting
        central.connect(p, options: nil)          // reconnect when back in range
    }

    fileprivate func handleServices(_ p: CBPeripheral) {
        let services = p.services ?? []
        if !services.contains(where: { $0.uuid == histService }) { status = .needsFirmware }
        for s in services {
            switch s.uuid {
            case histService: p.discoverCharacteristics([histInfo, histData, histCtrl], for: s)
            case hrService: p.discoverCharacteristics([hrMeasure], for: s)
            case battService: p.discoverCharacteristics([battLevel], for: s)
            case timeService: p.discoverCharacteristics([currentTime], for: s)
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
                p.readValue(for: c)
            case histData:
                dataChr = c
                p.setNotifyValue(true, for: c)
            case histCtrl:
                ctrlChr = c
            case hrMeasure:
                p.setNotifyValue(true, for: c)
            case battLevel, stepCount:
                p.readValue(for: c)
                if c.properties.contains(.notify) { p.setNotifyValue(true, for: c) }
            case currentTime:
                // keep the watch clock right; the watch only takes it from a bonded phone
                if c.properties.contains(.write) {
                    p.writeValue(Data(WatchValues.currentTime(Date())), for: c, type: .withResponse)
                }
            default: break
            }
        }
    }

    fileprivate func handleNotifyState(_ c: CBCharacteristic, error: Error?) {
        guard c.uuid == histData else { return }
        if let error {
            lastError = "History subscription failed: \(error.localizedDescription)"
            return
        }
        dataSubscribed = c.isNotifying
        startSyncIfReady()
    }

    fileprivate func handleValue(_ c: CBCharacteristic, error: Error?) {
        guard error == nil, let v = c.value else {
            if let error, c.uuid == histInfo {
                lastError = "Could not read the watch history: \(error.localizedDescription)"
            }
            return
        }
        let bytes = [UInt8](v)
        switch c.uuid {
        case histInfo:
            if let i = HistoryInfo(bytes) {
                info = i
                startSyncIfReady()
            }
        case histData:
            handle(batch: bytes)
        case hrMeasure:
            if let b = WatchValues.heartRate(bytes) { liveBPM = b > 0 ? b : nil }
        case battLevel:
            battery = bytes.first.map(Int.init)
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
        if let error, c.uuid == histCtrl {
            lastError = "The watch refused a sync command: \(error.localizedDescription)"
            sentSend = false
        }
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
        MainActor.assumeIsolated { handleServices(peripheral) }
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
