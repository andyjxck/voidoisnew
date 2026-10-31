import Foundation

public struct Workout: Identifiable, Hashable, Sendable {
    public var id: UInt32
    public var start: UInt32          // watch time
    public var activeSeconds: Int
    public var sets: Int
    public var routine: Int?
    public var avgBPM: Int
    public var maxBPM: Int
    public var calories: Int

    public var routineName: String {
        switch routine {
        case 0: return "Push"
        case 1: return "Pull"
        case 2: return "Legs"
        case nil: return "Quick workout"
        default: return "Routine \(routine! + 1)"
        }
    }
    public var day: Int { Int(start / 86400) }
}

public struct SleepSession: Identifiable, Hashable, Sendable {
    public var id: UInt32
    public var bed: UInt32            // watch time
    public var seconds: Int
    public var restlessMinutes: Int
    public var avgBPM: Int?
    public var lowBPM: Int?
    /// moving seconds per 5-minute slot across the night, for the timeline
    public var movement: [Int]
    public var heart: [Int]
    public var score: Int = 0

    public var wake: UInt32 { bed + UInt32(seconds) }
    public var minutes: Int { seconds / 60 }
    public var wakeDay: Int { Int(wake / 86400) }
    public var stillPercent: Int {
        minutes == 0 ? 0 : max(0, min(100, 100 - restlessMinutes * 100 / max(1, minutes)))
    }
    /// minutes after 18:00 the night before (bedtime) / after midnight (wake),
    /// so averages do not break across midnight
    public var bedMinute: Int { (Int(bed % 86400) / 60 - 18 * 60 + 1440) % 1440 }
    public var wakeMinute: Int { Int(wake % 86400) / 60 }
}

public struct HeartPoint: Hashable, Sendable {
    public var second: Int            // second of day
    public var avg: Int
    public var max: Int
}

public struct DaySummary: Identifiable, Hashable, Sendable {
    public var day: Int
    public var id: Int { day }
    public var steps = 0
    public var stepsFromSamples = 0
    public var stepsHourly = [Int](repeating: 0, count: 24)
    public var distanceKm = 0.0
    public var water = 0
    public var waterEvents: [(second: Int, delta: Int)] = []
    public var waterHourly = [Int](repeating: 0, count: 24)
    public var restingHR: Int?
    public var restingEstimated = false
    public var hrMin: Int?
    public var hrAvg: Int?
    public var hrMax: Int?
    public var heart: [HeartPoint] = []
    public var zoneMinutes: [HRZone: Int] = [:]
    public var activeMinutes = 0
    public var activeHourly = [Int](repeating: 0, count: 24)
    public var movingMinutes = 0
    public var caloriesTotal = 0
    public var caloriesActive = 0
    public var caloriesHourly = [Int](repeating: 0, count: 24)
    public var workouts: [Workout] = []
    public var sleep: [SleepSession] = []
    public var battery: [(second: Int, pct: Int, charging: Bool)] = []
    public var samples = 0
    public var clockGuess = false
    public var hasDayRecord = false

    public init(day: Int) { self.day = day }

    public var mainSleep: SleepSession? { sleep.max { $0.seconds < $1.seconds } }
    public var sleepMinutes: Int { sleep.reduce(0) { $0 + $1.minutes } }
    public var workoutMinutes: Int { workouts.reduce(0) { $0 + $1.activeSeconds } / 60 }
    public var cardioMinutes: Int { (zoneMinutes[.cardio] ?? 0) + (zoneMinutes[.peak] ?? 0) }
    public var zoneTotal: Int { HRZone.allCases.filter { $0 != .below }.reduce(0) { $0 + (zoneMinutes[$1] ?? 0) } }
    public var hasData: Bool {
        steps > 0 || water > 0 || samples > 0 || !workouts.isEmpty || !sleep.isEmpty || hasDayRecord
    }

    public static func == (a: DaySummary, b: DaySummary) -> Bool {
        a.day == b.day && a.steps == b.steps && a.samples == b.samples && a.water == b.water &&
            a.workouts == b.workouts && a.sleep == b.sleep && a.caloriesTotal == b.caloriesTotal
    }
    public func hash(into h: inout Hasher) { h.combine(day); h.combine(samples); h.combine(steps) }
}

/// Everything the app shows, derived from the stored records.
public struct HealthModel: Sendable {
    public var days: [Int: DaySummary] = [:]
    public var profile: Profile
    public var today: Int
    public var firstDay: Int?

    public init(profile: Profile, today: Int) {
        self.profile = profile
        self.today = today
    }

    public func summary(_ day: Int) -> DaySummary { days[day] ?? DaySummary(day: day) }

    /// - live: today's step count read from the watch just now, if any
    public static func build(records: [WatchRecord], profile: Profile, today: Int,
                             nowSecond: Int, liveSteps: Int? = nil) -> HealthModel {
        var m = HealthModel(profile: profile, today: today)
        let sorted = records.sorted { $0.t != $1.t ? $0.t < $1.t : $0.seq < $1.seq }
        var samples: [Int: [WatchRecord]] = [:]
        var dayRec: [Int: WatchRecord] = [:]
        var water: [Int: [WatchRecord]] = [:]
        var workouts: [WatchRecord] = []
        var sleeps: [WatchRecord] = []
        var allSamples: [WatchRecord] = []
        for r in sorted {
            switch r.kind {
            case .sample: samples[r.day, default: []].append(r); allSamples.append(r)
            case .day: dayRec[r.day] = r
            case .water: water[r.day, default: []].append(r)
            case .workout: workouts.append(r)
            case .sleep: sleeps.append(r)
            case nil: break
            }
        }
        let keys = Set(samples.keys).union(dayRec.keys).union(water.keys)
            .union(workouts.map(\.day)).union(sleeps.map { Int(($0.t + $0.b) / 86400) })
        let maxHR = profile.maxHR
        let kcalPerStep = profile.weightKg * 0.0005

        for day in keys {
            var d = DaySummary(day: day)
            let ss = samples[day] ?? []
            d.samples = ss.count
            var activeSlots = Set<Int>()
            var hrSum = 0, hrN = 0
            var lowStill: Int?
            for s in ss {
                let sec = s.secondOfDay, hour = sec / 3600, slot = sec / 300
                d.stepsFromSamples += s.steps
                d.stepsHourly[hour] += s.steps
                d.movingMinutes += s.movingSeconds
                if s.clockGuess { d.clockGuess = true }
                if s.battery > 0 || s.charging {
                    d.battery.append((sec, s.battery, s.charging))
                }
                var active = s.steps >= 500
                var kcal = Double(s.steps) * kcalPerStep
                if s.avgBPM > 0 {
                    d.heart.append(HeartPoint(second: sec, avg: s.avgBPM, max: max(s.avgBPM, s.maxBPM)))
                    hrSum += s.avgBPM
                    hrN += 1
                    d.hrMin = min(d.hrMin ?? 999, s.avgBPM)
                    d.hrMax = max(d.hrMax ?? 0, max(s.avgBPM, s.maxBPM))
                    let z = HRZone.of(bpm: s.avgBPM, maxHR: maxHR)
                    if z != .below { d.zoneMinutes[z, default: 0] += 5 }
                    if Double(s.avgBPM) >= 0.64 * Double(maxHR) { active = true }
                    if s.avgBPM >= 90 {
                        kcal = max(kcal, Self.keytelActive(bpm: s.avgBPM, profile: profile) * 5)
                    }
                    if s.movingSeconds <= 10 { lowStill = min(lowStill ?? 999, s.avgBPM) }
                }
                if active { activeSlots.insert(slot) }
                d.caloriesHourly[hour] += Int(kcal.rounded())
            }
            d.movingMinutes /= 60
            if hrN > 0 { d.hrAvg = (hrSum + hrN / 2) / hrN }

            if let r = dayRec[day] {
                d.hasDayRecord = true
                d.steps = max(r.daySteps, d.stepsFromSamples)
                d.water = r.dayWater
                if r.dayRestBPM > 0 { d.restingHR = r.dayRestBPM }
                if r.clockGuess { d.clockGuess = true }
            } else {
                d.steps = d.stepsFromSamples
            }
            if day == today, let live = liveSteps, live > d.steps { d.steps = live }
            if d.restingHR == nil, let low = lowStill {
                d.restingHR = low
                d.restingEstimated = true
            }
            for w in water[day] ?? [] {
                d.waterEvents.append((w.secondOfDay, w.waterDelta))
                d.waterHourly[w.secondOfDay / 3600] += w.waterDelta
                if !d.hasDayRecord || day == today { d.water = max(0, w.waterTotal) }
            }
            for w in workouts where w.day == day {
                let kcal = w.workoutAvgBPM >= 90
                    ? Self.keytelActive(bpm: w.workoutAvgBPM, profile: profile) * Double(w.activeSeconds) / 60
                    : Double(w.activeSeconds) / 60 * 3.5 * profile.weightKg / 200   // ~3.5 MET
                d.workouts.append(Workout(id: w.seq, start: w.t, activeSeconds: w.activeSeconds,
                                          sets: w.sets, routine: w.routine, avgBPM: w.workoutAvgBPM,
                                          maxBPM: w.workoutMaxBPM, calories: Int(kcal.rounded())))
                let s0 = w.secondOfDay / 300
                let s1 = min(287, (w.secondOfDay + w.activeSeconds) / 300)
                if s1 >= s0 { for s in s0...s1 { activeSlots.insert(s) } }
            }
            d.activeMinutes = activeSlots.count * 5
            for s in activeSlots { d.activeHourly[s / 12] += 5 }
            d.distanceKm = Double(d.steps) * profile.strideMetres / 1000
            // steps the samples missed (clock unknown for a while) still burn energy
            let missed = max(0, d.steps - d.stepsFromSamples)
            let active = d.caloriesHourly.reduce(0, +) + Int((Double(missed) * kcalPerStep).rounded())
            let fraction = day == today ? Double(max(0, min(86400, nowSecond))) / 86400 : 1
            d.caloriesActive = active
            d.caloriesTotal = Int((profile.bmr * fraction).rounded()) + active
            m.days[day] = d
        }

        // nights: analyse the samples between bedtime and wake
        var nights: [SleepSession] = []
        for r in sleeps where r.secondsInBed > 0 {
            let wake = r.t + r.b
            var lo = 0, hi = allSamples.count       // first sample in the night
            while lo < hi {
                let mid = (lo + hi) / 2
                if allSamples[mid].t + 150 < r.t { lo = mid + 1 } else { hi = mid }
            }
            var inside: [WatchRecord] = []
            var k = lo
            while k < allSamples.count, allSamples[k].t + 150 < wake {
                inside.append(allSamples[k])
                k += 1
            }
            var s = SleepSession(id: r.seq, bed: r.t, seconds: r.secondsInBed, restlessMinutes: 0,
                                 avgBPM: nil, lowBPM: nil, movement: [], heart: [])
            var sum = 0, n = 0
            for x in inside {
                s.movement.append(x.movingSeconds)
                s.heart.append(x.avgBPM)
                if x.movingSeconds >= 30 { s.restlessMinutes += 5 }
                if x.avgBPM > 0 {
                    sum += x.avgBPM
                    n += 1
                    s.lowBPM = min(s.lowBPM ?? 999, x.avgBPM)
                }
            }
            s.restlessMinutes = min(s.restlessMinutes, s.minutes)
            if n > 0 { s.avgBPM = (sum + n / 2) / n }
            nights.append(s)
        }
        nights.sort { $0.bed < $1.bed }
        for i in nights.indices {
            let prev = nights[max(0, i - 7)..<i]
            nights[i].score = Self.sleepScore(nights[i], previous: Array(prev), goal: profile.sleepGoalMinutes)
            let wd = nights[i].wakeDay
            var d = m.days[wd] ?? DaySummary(day: wd)
            d.sleep.append(nights[i])
            m.days[wd] = d
        }
        m.firstDay = m.days.keys.min()
        return m
    }

    /// energy above resting per minute from heart rate (Keytel et al. 2005)
    static func keytelActive(bpm: Int, profile: Profile) -> Double {
        let hr = Double(bpm), w = profile.weightKg, a = Double(profile.age)
        let male = (-55.0969 + 0.6309 * hr + 0.1988 * w + 0.2017 * a) / 4.184
        let female = (-20.4022 + 0.4472 * hr - 0.1263 * w + 0.074 * a) / 4.184
        let gross: Double
        switch profile.sex {
        case .male: gross = male
        case .female: gross = female
        case .other: gross = (male + female) / 2
        }
        return max(0, gross - profile.bmr / 1440)
    }

    /// 0-100: duration against the goal (50), how still the night was (25),
    /// bedtime close to the previous week's average (25)
    static func sleepScore(_ s: SleepSession, previous: [SleepSession], goal: Int) -> Int {
        let dur = min(1, Double(s.minutes) / Double(max(60, goal))) * 50
        let still = Double(s.stillPercent) / 100 * 25
        var cons = 25.0
        if !previous.isEmpty {
            let avg = Double(previous.map(\.bedMinute).reduce(0, +)) / Double(previous.count)
            cons = max(0, 1 - abs(Double(s.bedMinute) - avg) / 90) * 25
        }
        return Int((dur + still + cons).rounded())
    }
}
