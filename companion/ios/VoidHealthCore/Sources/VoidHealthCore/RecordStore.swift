import Foundation

/// The phone's copy of everything the watch sent: an append-only file of
/// 20-byte entries (log id + the record exactly as the watch sent it).
/// `add` returns only after the bytes are on disk, so acknowledging the
/// watch after it returns can never lose a record. Duplicates (the watch
/// sending again after a lost ack) are dropped by log id + seq.
public final class RecordStore: @unchecked Sendable {
    public private(set) var records: [WatchRecord] = []
    private var seen = Set<UInt64>()
    private var lastSeq: [UInt32: UInt32] = [:]
    private let url: URL
    private let lock = NSLock()

    public static let entrySize = 20

    public init(url: URL) throws {
        self.url = url
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: url.path) {
            _ = FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        let data = try Data(contentsOf: url)
        let bytes = [UInt8](data)
        let whole = bytes.count / Self.entrySize * Self.entrySize
        var i = 0
        while i < whole {
            let log = le32(bytes, i)
            if let r = WatchRecord.decode(bytes[(i + 4)..<(i + 20)]) {
                insert(r, log: log)
            }
            i += Self.entrySize
        }
        if whole != bytes.count {           // torn tail from a crash mid-write
            let h = try FileHandle(forWritingTo: url)
            try h.truncate(atOffset: UInt64(whole))
            try h.close()
        }
    }

    @discardableResult
    private func insert(_ r: WatchRecord, log: UInt32) -> Bool {
        let key = UInt64(log) << 32 | UInt64(r.seq)
        guard seen.insert(key).inserted else { return false }
        records.append(r)
        if r.seq > lastSeq[log] ?? 0 { lastSeq[log] = r.seq }
        return true
    }

    /// highest seq held from this watch log
    public func last(log: UInt32) -> UInt32? {
        lock.lock(); defer { lock.unlock() }
        return lastSeq[log]
    }

    /// saves new records durably; returns the ones that were new
    @discardableResult
    public func add(_ batch: [WatchRecord], log: UInt32) throws -> [WatchRecord] {
        lock.lock(); defer { lock.unlock() }
        var fresh: [WatchRecord] = []
        var out: [UInt8] = []
        var keys = Set<UInt64>()
        for r in batch {
            let key = UInt64(log) << 32 | UInt64(r.seq)
            if seen.contains(key) || !keys.insert(key).inserted { continue }
            var e = [UInt8](repeating: 0, count: 4)
            put32(&e, 0, log)
            out += e + r.encoded()
            fresh.append(r)
        }
        guard !fresh.isEmpty else { return [] }
        let h = try FileHandle(forWritingTo: url)
        defer { try? h.close() }
        try h.seekToEnd()
        try h.write(contentsOf: Data(out))
        try h.synchronize()
        for r in fresh { insert(r, log: log) }
        return fresh
    }

    public var count: Int {
        lock.lock(); defer { lock.unlock() }
        return records.count
    }

    public func snapshot() -> [WatchRecord] {
        lock.lock(); defer { lock.unlock() }
        return records
    }

    /// wipes the phone's history (the watch is not touched)
    public func eraseAll() throws {
        lock.lock(); defer { lock.unlock() }
        try Data().write(to: url)
        records = []
        seen = []
        lastSeq = [:]
    }
}
