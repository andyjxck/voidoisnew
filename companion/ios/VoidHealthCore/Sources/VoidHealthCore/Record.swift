import Foundation

/// One 16-byte record from the watch's history outbox (firmware/hlog.h).
/// `t` is seconds since 2020-01-01 00:00 in the watch's LOCAL wall time,
/// so `t / 86400` is the local day and `t % 86400` the time of day.
public struct WatchRecord: Hashable, Codable, Sendable {
    public var seq: UInt32
    public var t: UInt32
    public var type: UInt8
    public var a: UInt16
    public var b: UInt32

    public init(seq: UInt32, t: UInt32, type: UInt8, a: UInt16, b: UInt32) {
        self.seq = seq
        self.t = t
        self.type = type
        self.a = a
        self.b = b
    }

    public enum Kind: UInt8, Sendable {
        case sample = 1, water = 2, workout = 3, sleep = 4, day = 5
    }

    public var kind: Kind? { Kind(rawValue: type & 0x7F) }
    /// The watch's clock was restored from flash, not set by the phone.
    public var clockGuess: Bool { type & 0x80 != 0 }
    public var day: Int { Int(t / 86400) }
    public var secondOfDay: Int { Int(t % 86400) }

    // MARK: wire format

    public static let size = 16

    public static func crc8(_ bytes: [UInt8]) -> UInt8 {
        var c: UInt8 = 0xFF
        for x in bytes {
            c ^= x
            for _ in 0..<8 { c = c & 0x80 != 0 ? (c << 1) ^ 0x07 : c << 1 }
        }
        return c
    }

    /// nil for an erased slot or a bad checksum
    public static func decode<C: Collection>(_ data: C) -> WatchRecord? where C.Element == UInt8 {
        let b = Array(data)
        guard b.count == size else { return nil }
        let seq = le32(b, 0), t = le32(b, 4)
        guard seq != 0, seq != .max, b[8] != 0xFF else { return nil }
        guard crc8(Array(b[0..<9]) + Array(b[10..<16])) == b[9] else { return nil }
        return WatchRecord(seq: seq, t: t, type: b[8],
                           a: UInt16(b[10]) | UInt16(b[11]) << 8, b: le32(b, 12))
    }

    public func encoded() -> [UInt8] {
        var o = [UInt8](repeating: 0, count: 16)
        put32(&o, 0, seq)
        put32(&o, 4, t)
        o[8] = type
        o[10] = UInt8(a & 0xFF)
        o[11] = UInt8(a >> 8)
        put32(&o, 12, b)
        o[9] = Self.crc8(Array(o[0..<9]) + Array(o[10..<16]))
        return o
    }

    /// a notification carries 1..15 records back to back
    public static func decodeBatch(_ data: [UInt8]) -> [WatchRecord] {
        stride(from: 0, to: data.count - size + 1, by: size).compactMap {
            decode(data[$0..<($0 + size)])
        }
    }

    // MARK: typed views

    /// 5-minute sample
    public var steps: Int { Int(a) }
    public var avgBPM: Int { Int(b & 0xFF) }
    public var maxBPM: Int { Int((b >> 8) & 0xFF) }
    public var battery: Int { Int((b >> 16) & 0x7F) }
    public var charging: Bool { (b >> 23) & 1 == 1 }
    public var movingSeconds: Int { Int((b >> 24) & 0xFF) * 2 }

    /// water tap
    public var waterDelta: Int { Int(Int16(bitPattern: a)) }
    public var waterTotal: Int { Int(b) }

    /// workout (t = start)
    public var activeSeconds: Int { Int(a) }
    public var sets: Int { Int(b & 0xFF) }
    /// 0 PUSH, 1 PULL, 2 LEGS, nil = quick workout
    public var routine: Int? { let r = Int((b >> 8) & 0xFF); return r == 0xFF ? nil : r }
    public var workoutAvgBPM: Int { Int((b >> 16) & 0xFF) }
    public var workoutMaxBPM: Int { Int(b >> 24) }

    /// sleep (t = bedtime, or when sleep began for a detected night)
    public var secondsInBed: Int { Int(b) }
    /// sleep: how the night was found (VoidOS 2.4.2). Bit 0 detected
    /// automatically, bit 1 nap, bits 4-15 minutes awake inside it.
    /// 0 for a BEDTIME / WAKE UP night and for older firmware.
    public var sleepAuto: Bool { a & 0x1 != 0 }
    public var sleepNap: Bool { a & 0x2 != 0 }
    public var sleepAwakeMinutes: Int { Int(a >> 4) }

    /// day totals (t = local midnight)
    public var dayWater: Int { Int(a) }
    public var daySteps: Int { Int(b & 0xFFFFFF) }
    public var dayRestBPM: Int { Int(b >> 24) }
}

func le32(_ b: [UInt8], _ i: Int) -> UInt32 {
    UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24
}

func put32(_ o: inout [UInt8], _ i: Int, _ v: UInt32) {
    o[i] = UInt8(v & 0xFF)
    o[i + 1] = UInt8((v >> 8) & 0xFF)
    o[i + 2] = UInt8((v >> 16) & 0xFF)
    o[i + 3] = UInt8(v >> 24)
}
