import XCTest
@testable import VoidHealthCore

final class RecordTests: XCTestCase {
    /// produced by the firmware's own hrec_pack (firmware/hlog.c)
    static let fromFirmware = [
        "010000000047b61101cd9c01485fd125",
        "020000008855b611822506ffe2040000",
        "03000000a0d3b61103238d0e0e0279a8",
        "0400000070c8b5110408000078690000",
        "0500000080d6b5110500d60639300036",
    ]

    static func hex(_ s: String) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return out
    }

    func testDecodesFirmwareRecords() throws {
        let rs = Self.fromFirmware.map { WatchRecord.decode(Self.hex($0)) }
        XCTAssertEqual(rs.compactMap { $0 }.count, 5)
        let s = try XCTUnwrap(rs[0])
        XCTAssertEqual(s.kind, .sample)
        XCTAssertEqual(s.steps, 412)
        XCTAssertEqual(s.avgBPM, 72)
        XCTAssertEqual(s.maxBPM, 95)
        XCTAssertEqual(s.battery, 81)
        XCTAssertTrue(s.charging)
        XCTAssertEqual(s.movingSeconds, 74)
        XCTAssertEqual(s.secondOfDay, 8 * 3600)
        let w = try XCTUnwrap(rs[1])
        XCTAssertEqual(w.kind, .water)
        XCTAssertTrue(w.clockGuess)
        XCTAssertEqual(w.waterDelta, -250)
        XCTAssertEqual(w.waterTotal, 1250)
        let k = try XCTUnwrap(rs[2])
        XCTAssertEqual(k.kind, .workout)
        XCTAssertEqual(k.activeSeconds, 3725)
        XCTAssertEqual(k.sets, 14)
        XCTAssertEqual(k.routine, 2)
        XCTAssertEqual(k.workoutAvgBPM, 121)
        XCTAssertEqual(k.workoutMaxBPM, 168)
        let sl = try XCTUnwrap(rs[3])
        XCTAssertEqual(sl.kind, .sleep)
        XCTAssertEqual(sl.secondsInBed, 27000)
        let d = try XCTUnwrap(rs[4])
        XCTAssertEqual(d.kind, .day)
        XCTAssertEqual(d.dayWater, 1750)
        XCTAssertEqual(d.daySteps, 12345)
        XCTAssertEqual(d.dayRestBPM, 54)
    }

    func testEncodeMatchesFirmware() {
        for h in Self.fromFirmware {
            let r = WatchRecord.decode(Self.hex(h))!
            XCTAssertEqual(r.encoded(), Self.hex(h))
        }
    }

    func testRejectsCorruptAndErased() {
        var b = Self.hex(Self.fromFirmware[0])
        b[5] ^= 0x10
        XCTAssertNil(WatchRecord.decode(b))
        XCTAssertNil(WatchRecord.decode([UInt8](repeating: 0xFF, count: 16)))
    }

    func testBatch() {
        let all = Self.fromFirmware.flatMap(Self.hex)
        XCTAssertEqual(WatchRecord.decodeBatch(all).map(\.seq), [1, 2, 3, 4, 5])
    }
}

final class ProtocolTests: XCTestCase {
    func info(first: UInt32, next: UInt32, acked: UInt32) -> HistoryInfo {
        var b: [UInt8] = [1, 16, 255, 0]
        for v in [0xABCD_1234, first, next, acked, 0, 65280] as [UInt32] {
            b += [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)]
        }
        return HistoryInfo(b)!
    }

    func testInfoAndResume() {
        let i = info(first: 511, next: 801, acked: 600)
        XCTAssertEqual(i.logID, 0xABCD_1234)
        XCTAssertEqual(i.pending, 200)
        XCTAssertEqual(HistoryControl.resumeSeq(phoneLast: nil, info: i), 511)
        XCTAssertEqual(HistoryControl.resumeSeq(phoneLast: 700, info: i), 701)
        XCTAssertEqual(HistoryControl.resumeSeq(phoneLast: 100, info: i), 511)   // gap: take what is left
        XCTAssertEqual(HistoryControl.resumeSeq(phoneLast: 5000, info: i), 511)  // watch log restarted
        XCTAssertEqual(HistoryControl.send(from: 0x0102_0304), [1, 4, 3, 2, 1])
        XCTAssertEqual(HistoryControl.ack(through: 7), [2, 7, 0, 0, 0])
    }

    func testCurrentTime() {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "Europe/London")!
        let d = cal.date(from: DateComponents(year: 2026, month: 10, day: 31, hour: 13, minute: 5, second: 9))!
        XCTAssertEqual(WatchValues.currentTime(d, calendar: cal), [0xEA, 0x07, 10, 31, 13, 5, 9, 6, 0, 1])
    }

    func testHeartRateAndSteps() {
        XCTAssertEqual(WatchValues.heartRate([0, 72]), 72)
        XCTAssertEqual(WatchValues.heartRate([1, 0x2C, 0x01]), 300)
        XCTAssertEqual(WatchValues.steps([0x10, 0x27, 0, 0]), 10000)
    }
}

final class StoreTests: XCTestCase {
    func testDurableAndDeduplicated() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let url = dir.appendingPathComponent("records.bin")
        let s = try RecordStore(url: url)
        let recs = (1...10).map { WatchRecord(seq: UInt32($0), t: UInt32($0) * 300, type: 1, a: 5, b: 70) }
        XCTAssertEqual(try s.add(recs, log: 7).count, 10)
        XCTAssertEqual(try s.add(Array(recs[5...]) + [recs[0]], log: 7).count, 0)     // resent after a lost ack
        XCTAssertEqual(try s.add(recs, log: 8).count, 10)                             // a new watch log
        XCTAssertEqual(s.last(log: 7), 10)
        // torn tail from a crash is dropped on load
        let h = try FileHandle(forWritingTo: url)
        try h.seekToEnd()
        try h.write(contentsOf: Data([1, 2, 3]))
        try h.close()
        let again = try RecordStore(url: url)
        XCTAssertEqual(again.count, 20)
        XCTAssertEqual(again.last(log: 8), 10)
        XCTAssertEqual(try Data(contentsOf: url).count, 20 * 20)
    }
}

final class ModelTests: XCTestCase {
    var profile: Profile = {
        var p = Profile()
        p.age = 30
        p.sex = .male
        p.weightKg = 80
        p.heightCm = 180
        return p
    }()

    var seq: UInt32 = 0
    func rec(_ day: Int, _ sec: Int, _ type: UInt8, a: Int = 0, b: UInt32 = 0) -> WatchRecord {
        seq += 1
        return WatchRecord(seq: seq, t: UInt32(day * 86400 + sec), type: type, a: UInt16(truncatingIfNeeded: a), b: b)
    }
    func sample(_ day: Int, _ sec: Int, steps: Int, bpm: Int, max: Int = 0, moving: Int = 0, batt: Int = 80) -> WatchRecord {
        rec(day, sec, 1, a: steps, b: UInt32(bpm) | UInt32(max) << 8 | UInt32(batt) << 16 | UInt32(moving / 2) << 24)
    }

    func testDayTotalsAndDerived() {
        let day = 2400
        var rs: [WatchRecord] = []
        // a brisk 30-minute walk at 9:00, slow wandering otherwise
        for i in 0..<6 { rs.append(sample(day, 9 * 3600 + i * 300, steps: 600, bpm: 110, max: 120, moving: 280)) }
        for i in 0..<12 { rs.append(sample(day, 14 * 3600 + i * 300, steps: 50, bpm: 64, moving: 4)) }
        rs.append(rec(day, 10 * 3600, 2, a: 250, b: 250))
        rs.append(rec(day, 11 * 3600, 2, a: 500, b: 750))
        rs.append(rec(day, 18 * 3600, 3, a: 2700, b: 12 | 0 << 8 | 130 << 16 | 165 << 24))
        rs.append(rec(day, 0, 5, a: 750, b: 5000 | 58 << 24))         // DAY totals, t = that day's midnight
        let m = HealthModel.build(records: rs, profile: profile, today: day + 1, nowSecond: 3600)
        let s = m.summary(day)
        XCTAssertEqual(s.steps, 5000)                                  // the watch's total wins over samples
        XCTAssertEqual(m.summary(day + 1).hasData, false)
        XCTAssertEqual(s.stepsFromSamples, 4200)
        XCTAssertEqual(s.stepsHourly[9], 3600)
        XCTAssertEqual(s.water, 750)
        XCTAssertEqual(s.hrMax, 120)
        XCTAssertEqual(s.restingHR, 58)
        XCTAssertFalse(s.restingEstimated)
        // 6 walking slots (30 min) + workout 18:00-18:45 (10 slots incl. the end) = 80
        XCTAssertEqual(s.activeMinutes, 30 + 50)
        XCTAssertEqual(s.zoneMinutes[.fatBurn], 30)        // 110 bpm of 190 max
        XCTAssertEqual(s.workouts.first?.routineName, "Push")
        XCTAssertEqual(s.workouts.first?.sets, 12)
        XCTAssertGreaterThan(s.caloriesTotal, Int(profile.bmr))
        XCTAssertEqual(s.distanceKm, 5000 * 1.8 * 0.415 / 1000, accuracy: 0.001)
    }

    func testSleepAndStreaks() {
        var rs: [WatchRecord] = []
        let start = 3000
        for n in 0..<10 {
            let day = start + n
            rs.append(rec(day, 12 * 3600, 1, a: 100, b: 70))            // a sample so the day has data
            rs.append(rec(day, 0, 5, a: 2000, b: UInt32(n == 4 ? 3000 : 11000) | 60 << 24))
            // bed at 23:00, 7.5 h, restless for the first 3 slots
            let bed = day * 86400 + 23 * 3600
            rs.append(WatchRecord(seq: { seq += 1; return seq }(), t: UInt32(bed), type: 4, a: 0, b: 27000))
            for i in 0..<90 {
                let t = bed + i * 300
                rs.append(sample(t / 86400, t % 86400, steps: 0, bpm: 52, moving: i < 3 ? 120 : 0))
            }
        }
        let today = start + 10
        let m = HealthModel.build(records: rs, profile: profile, today: today, nowSecond: 8 * 3600)
        let night = m.summary(today).mainSleep!
        XCTAssertEqual(night.minutes, 450)
        XCTAssertEqual(night.restlessMinutes, 15)
        XCTAssertEqual(night.lowBPM, 52)
        XCTAssertGreaterThan(night.score, 80)
        let st = m.streak(.steps)
        XCTAssertEqual(st.longest, 5)                 // day start+4 is under the goal
        XCTAssertEqual(st.current, 5)                 // today is unfinished, so it does not break the run
        let c = m.sleepConsistency()!
        XCTAssertEqual(c.bedSD, 0)
        XCTAssertEqual(c.score, 100)
        let cal = DayCalendar()
        let week = m.series(.steps, span: .week, page: 0, cal: cal)
        XCTAssertEqual(week.count, 7)
        let year = m.series(.sleep, span: .year, page: 0, cal: cal)
        XCTAssertEqual(year.count, 12)
        XCTAssertFalse(m.personalBests(cal: cal).isEmpty)
    }

    func testAYearOfData() {
        var rs: [WatchRecord] = []
        rs.reserveCapacity(110_000)
        for day in 2000..<2365 {
            for slot in 0..<288 {
                rs.append(sample(day, slot * 300, steps: slot % 7 * 40, bpm: 55 + slot % 40, moving: slot % 5 * 20))
            }
            rs.append(rec(day, 0, 5, a: 1500, b: 9000 | 55 << 24))
            rs.append(WatchRecord(seq: { seq += 1; return seq }(), t: UInt32(day * 86400 + 23 * 3600), type: 4, a: 0, b: 28000))
        }
        let t0 = Date()
        let m = HealthModel.build(records: rs, profile: profile, today: 2365, nowSecond: 3600)
        let cal = DayCalendar()
        _ = m.series(.heartRate, span: .year, page: 0, cal: cal)
        _ = m.personalBests(cal: cal)
        let secs = Date().timeIntervalSince(t0)
        print("a year of records (\(rs.count)) built in \(secs) s")
        XCTAssertEqual(m.days.count, 366)
        XCTAssertLessThan(secs, 10)
    }

    func testCalendarRoundTrip() {
        let cal = DayCalendar()
        let d = cal.date(3439)
        XCTAssertEqual(cal.day(of: d), 3439)
        let comps = cal.calendar.dateComponents([.year, .month, .day], from: d)
        XCTAssertEqual([comps.year, comps.month, comps.day], [2029, 6, 1])
        let t = cal.date(watchTime: 3439 * 86400 + 8 * 3600 + 30 * 60)
        XCTAssertEqual(cal.calendar.component(.hour, from: t), 8)
        XCTAssertEqual(cal.calendar.component(.minute, from: t), 30)
        let m = cal.monthStart(3439 + 14)
        XCTAssertEqual(m, 3439)
        XCTAssertEqual(cal.addMonths(m, 1) - m, 30)   // June has 30 days
    }
}
