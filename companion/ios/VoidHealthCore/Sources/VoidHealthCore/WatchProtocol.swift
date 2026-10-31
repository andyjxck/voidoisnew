import Foundation

/// The VoidOS GATT services the app uses. See companion/PROTOCOL.md.
public enum WatchUUID {
    public static let historyService = "A3C10000-5F2E-4B8A-9D4E-766F69644F53"
    public static let historyInfo = "A3C10001-5F2E-4B8A-9D4E-766F69644F53"
    public static let historyData = "A3C10002-5F2E-4B8A-9D4E-766F69644F53"
    public static let historyControl = "A3C10003-5F2E-4B8A-9D4E-766F69644F53"
    public static let heartRateService = "180D"
    public static let heartRateMeasurement = "2A37"
    public static let batteryService = "180F"
    public static let batteryLevel = "2A19"
    public static let timeService = "1805"
    public static let currentTime = "2A2B"
    public static let deviceInfoService = "180A"
    public static let firmwareRevision = "2A26"
    public static let motionService = "00030000-78FC-48FE-8E23-433B3A1942D0"
    public static let stepCount = "00030001-78FC-48FE-8E23-433B3A1942D0"
    /// names VoidOS advertises (InfiniTime when the compatibility mode is on)
    public static let names = ["VoidOS", "InfiniTime", "Pinetime"]
}

/// INFO characteristic, 28 bytes
public struct HistoryInfo: Equatable, Sendable {
    public var proto: Int
    public var recordSize: Int
    public var perSector: Int
    public var logID: UInt32
    public var firstSeq: UInt32
    public var nextSeq: UInt32
    public var ackedSeq: UInt32
    public var dropped: UInt32
    public var capacity: UInt32

    public init?(_ data: [UInt8]) {
        guard data.count >= 28, data[0] == 1, data[1] == 16 else { return nil }
        proto = Int(data[0])
        recordSize = Int(data[1])
        perSector = Int(data[2]) | Int(data[3]) << 8
        logID = le32(data, 4)
        firstSeq = le32(data, 8)
        nextSeq = le32(data, 12)
        ackedSeq = le32(data, 16)
        dropped = le32(data, 20)
        capacity = le32(data, 24)
    }

    /// records still waiting on the watch
    public var pending: Int { max(0, Int(nextSeq) - max(Int(ackedSeq) + 1, Int(firstSeq))) }
    /// share of the watch's outbox in use
    public var fill: Double {
        capacity == 0 ? 0 : min(1, Double(max(0, Int(nextSeq) - Int(firstSeq))) / Double(capacity))
    }
}

public enum HistoryControl {
    public static func send(from seq: UInt32) -> [UInt8] { cmd(1, seq) }
    public static func ack(through seq: UInt32) -> [UInt8] { cmd(2, seq) }
    public static func stop() -> [UInt8] { cmd(3, 0) }
    /// VoidOS 2.4.0+: the daily goals the watch celebrates. Steps in bits
    /// 0-15, water (ml) in bits 16-31. Older firmware answers "request not
    /// supported", which the app ignores.
    public static func goals(steps: Int, waterML: Int) -> [UInt8] {
        let s = UInt32(min(max(steps, 0), 0xFFFF))
        let w = UInt32(min(max(waterML, 0), 0xFFFF))
        return cmd(goalsOp, s | w << 16)
    }
    public static let goalsOp: UInt8 = 4

    static func cmd(_ op: UInt8, _ v: UInt32) -> [UInt8] {
        var o: [UInt8] = [op, 0, 0, 0, 0]
        put32(&o, 1, v)
        return o
    }

    /// where to resume: after what the phone already holds, never before
    /// the oldest record the watch still has
    public static func resumeSeq(phoneLast: UInt32?, info: HistoryInfo) -> UInt32 {
        guard let last = phoneLast, last + 1 >= info.firstSeq, last < info.nextSeq else {
            return info.firstSeq
        }
        return last + 1
    }
}

/// Where a sync stands: what the phone has saved and what it has told the
/// watch it may erase. Pure bookkeeping so it can be tested off-device.
/// Acks only ever move forward, and only cover records already on disk.
public struct SyncCursor: Equatable, Sendable {
    public private(set) var highestSaved: UInt32 = 0
    public private(set) var lastAckSent: UInt32 = 0
    public private(set) var nextSeq: UInt32 = 0

    public init() {}

    /// a fresh INFO read: returns the seq to SEND from
    public mutating func start(info: HistoryInfo, phoneLast: UInt32?) -> UInt32 {
        let from = HistoryControl.resumeSeq(phoneLast: phoneLast, info: info)
        lastAckSent = info.ackedSeq
        nextSeq = info.nextSeq
        highestSaved = from > 0 ? from - 1 : 0
        // saved here before, but the ack never reached the watch
        if let held = phoneLast, held < info.nextSeq { highestSaved = max(highestSaved, held) }
        return from
    }

    /// records from this log are now on disk
    public mutating func saved(_ seqs: [UInt32]) {
        guard let top = seqs.max() else { return }
        if top > highestSaved { highestSaved = top }
        if highestSaved >= nextSeq { nextSeq = highestSaved + 1 }
    }

    /// a later INFO read while connected
    public mutating func update(_ info: HistoryInfo) {
        nextSeq = max(nextSeq, info.nextSeq)
        lastAckSent = max(lastAckSent, info.ackedSeq)
    }

    public var hasUnacked: Bool { highestSaved > lastAckSent }
    /// enough waiting that the ack should not wait for the batch to end
    public var ackIsDue: Bool { hasUnacked && highestSaved - lastAckSent >= 255 }
    public var upToDate: Bool { highestSaved + 1 >= nextSeq }

    /// the seq to ACK through, or nil when nothing new is saved
    public mutating func takeAck() -> UInt32? {
        guard hasUnacked else { return nil }
        lastAckSent = highestSaved
        return highestSaved
    }
}

public enum WatchValues {
    /// Current Time (0x2A2B), 10 bytes, local wall time
    public static func currentTime(_ date: Date, calendar: Calendar = .current) -> [UInt8] {
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second, .weekday], from: date)
        let y = c.year ?? 2026
        let wd = c.weekday.map { $0 == 1 ? 7 : $0 - 1 } ?? 0     // 1 = Monday
        return [UInt8(y & 0xFF), UInt8(y >> 8), UInt8(c.month ?? 1), UInt8(c.day ?? 1),
                UInt8(c.hour ?? 0), UInt8(c.minute ?? 0), UInt8(c.second ?? 0),
                UInt8(wd), 0, 1]           // fractions 0, reason: manual update
    }

    /// Heart Rate Measurement (0x2A37)
    public static func heartRate(_ d: [UInt8]) -> Int? {
        guard d.count >= 2 else { return nil }
        if d[0] & 1 == 1 {
            guard d.count >= 3 else { return nil }
            return Int(d[1]) | Int(d[2]) << 8
        }
        return Int(d[1])
    }

    /// InfiniTime step count, u32 little-endian
    public static func steps(_ d: [UInt8]) -> Int? {
        guard d.count >= 4 else { return nil }
        return Int(le32(d, 0))
    }
}
