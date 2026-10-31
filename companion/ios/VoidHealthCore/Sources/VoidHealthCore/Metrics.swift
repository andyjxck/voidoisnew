import Foundation

/// Watch days are counted from 2020-01-01 in local time.
public struct DayCalendar: Sendable {
    public var calendar: Calendar
    private let epoch: Date

    public init(calendar: Calendar = .current) {
        var c = calendar
        c.locale = calendar.locale ?? .current
        self.calendar = c
        epoch = c.date(from: DateComponents(year: 2020, month: 1, day: 1))!
    }

    public func date(_ day: Int) -> Date {
        calendar.date(byAdding: .day, value: day, to: epoch)!
    }
    /// a watch timestamp as a Date
    public func date(watchTime t: UInt32) -> Date {
        let start = date(Int(t / 86400))
        let s = Int(t % 86400)
        return calendar.date(bySettingHour: s / 3600, minute: s % 3600 / 60, second: s % 60, of: start) ?? start
    }
    public func day(of date: Date) -> Int {
        calendar.dateComponents([.day], from: epoch, to: calendar.startOfDay(for: date)).day ?? 0
    }
    public func secondOfDay(_ date: Date) -> Int {
        Int(date.timeIntervalSince(calendar.startOfDay(for: date)))
    }
    /// first day of the month containing day
    public func monthStart(_ day: Int) -> Int {
        let c = calendar.dateComponents([.year, .month], from: date(day))
        return self.day(of: calendar.date(from: c)!)
    }
    public func addMonths(_ day: Int, _ n: Int) -> Int {
        self.day(of: calendar.date(byAdding: .month, value: n, to: date(day))!)
    }
    /// first day of the week containing day (locale's first weekday)
    public func weekStart(_ day: Int) -> Int {
        let d = date(day)
        let wd = calendar.component(.weekday, from: d)
        return day - (wd - calendar.firstWeekday + 7) % 7
    }
}

public enum Metric: String, CaseIterable, Identifiable, Sendable {
    case steps, distance, calories, activeMinutes, heartRate, restingHR, zoneMinutes
    case sleep, sleepScore, water, workouts, battery

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .steps: return "Steps"
        case .distance: return "Distance"
        case .calories: return "Calories"
        case .activeMinutes: return "Active minutes"
        case .heartRate: return "Heart rate"
        case .restingHR: return "Resting heart rate"
        case .zoneMinutes: return "Heart zone minutes"
        case .sleep: return "Sleep"
        case .sleepScore: return "Sleep score"
        case .water: return "Water"
        case .workouts: return "Workouts"
        case .battery: return "Watch battery"
        }
    }

    /// daily metrics add up over a period; the rest are averaged
    public var isTotal: Bool {
        switch self {
        case .steps, .distance, .calories, .activeMinutes, .zoneMinutes, .water, .workouts: return true
        default: return false
        }
    }

    /// smaller is better (for personal bests)
    public var lowerIsBetter: Bool { self == .restingHR }

    public func value(_ d: DaySummary, profile: Profile) -> Double? {
        switch self {
        case .steps: return d.steps > 0 || d.hasData ? Double(d.steps) : nil
        case .distance:
            let km = d.distanceKm
            return d.hasData ? (profile.useMetric ? km : km / 1.609344) : nil
        case .calories: return d.hasData ? Double(d.caloriesTotal) : nil
        case .activeMinutes: return d.hasData ? Double(d.activeMinutes) : nil
        case .heartRate: return d.hrAvg.map(Double.init)
        case .restingHR: return d.restingHR.map(Double.init)
        case .zoneMinutes: return d.samples > 0 ? Double(d.zoneTotal) : nil
        case .sleep: return d.sleep.isEmpty ? nil : Double(d.sleepMinutes)
        case .sleepScore: return d.mainSleep.map { Double($0.score) }
        case .water: return d.hasData ? Double(d.water) : nil
        case .workouts: return d.workouts.isEmpty ? (d.hasData ? 0 : nil) : Double(d.workoutMinutes)
        case .battery: return d.battery.isEmpty ? nil : Double(d.battery.map(\.pct).min()!)
        }
    }

    public func goal(_ p: Profile) -> Double? {
        switch self {
        case .steps: return Double(p.stepGoal)
        case .activeMinutes: return Double(p.activeMinuteGoal)
        case .water: return Double(p.waterGoalML)
        case .sleep: return Double(p.sleepGoalMinutes)
        case .calories: return p.calorieGoal > 0 ? Double(p.calorieGoal) : nil
        default: return nil
        }
    }

    public func unit(_ p: Profile) -> String {
        switch self {
        case .steps: return "steps"
        case .distance: return p.useMetric ? "km" : "mi"
        case .calories: return "kcal"
        case .activeMinutes, .zoneMinutes, .workouts: return "min"
        case .heartRate, .restingHR: return "bpm"
        case .sleep: return ""
        case .sleepScore: return ""
        case .water: return "ml"
        case .battery: return "%"
        }
    }

    public func format(_ v: Double, profile p: Profile) -> String {
        switch self {
        case .distance: return String(format: "%.2f %@", v, unit(p))
        case .sleep: return Self.hm(Int(v.rounded()))
        case .water:
            return v >= 1000 ? String(format: "%.2f L", v / 1000) : "\(Int(v.rounded())) ml"
        case .sleepScore: return "\(Int(v.rounded()))"
        default:
            let n = Int(v.rounded())
            let s = NumberFormatter.localizedString(from: NSNumber(value: n), number: .decimal)
            let u = unit(p)
            return u.isEmpty ? s : "\(s) \(u)"
        }
    }

    public static func hm(_ minutes: Int) -> String {
        minutes >= 60 ? "\(minutes / 60)h \(minutes % 60)m" : "\(minutes)m"
    }
}

public enum Span: String, CaseIterable, Identifiable, Sendable {
    case day = "D", week = "W", month = "M", year = "Y"
    public var id: String { rawValue }
}

public struct Bucket: Identifiable, Hashable, Sendable {
    public var id: Int { index }
    public var index: Int
    public var date: Date
    public var value: Double?
    public var low: Double?
    public var high: Double?
}

public struct PeriodStats: Sendable {
    public var title: String = ""
    public var average: Double?
    public var total: Double?
    public var best: Double?
    public var bestDay: Int?
    public var daysWithData = 0
    public var goalDays = 0
    public var previousAverage: Double?
    /// change against the period before, in percent
    public var change: Double? {
        guard let a = average, let p = previousAverage, p > 0 else { return nil }
        return (a - p) / p * 100
    }
}

public struct Streak: Sendable {
    public var current = 0
    public var longest = 0
    public var longestEnd: Int?
}

public struct PersonalBest: Identifiable, Sendable {
    public var id: String { title }
    public var title: String
    public var value: String
    public var day: Int?
}

extension HealthModel {
    /// the days a span covers, `page` periods back from today (0 = current)
    public func period(_ span: Span, page: Int, cal: DayCalendar) -> ClosedRange<Int> {
        switch span {
        case .day:
            return (today - page)...(today - page)
        case .week:
            let s = cal.weekStart(today) - 7 * page
            return s...(s + 6)
        case .month:
            let s = cal.addMonths(cal.monthStart(today), -page)
            return s...(cal.addMonths(s, 1) - 1)
        case .year:
            let m = cal.monthStart(today)
            let s = cal.addMonths(m, -11 - 12 * page)
            return s...(cal.addMonths(s, 12) - 1)
        }
    }

    /// chart buckets: hours (or 5-minute slots) for a day, days for a week or
    /// month, months for a year (each month's daily average or total)
    public func series(_ metric: Metric, span: Span, page: Int, cal: DayCalendar) -> [Bucket] {
        let range = period(span, page: page, cal: cal)
        switch span {
        case .day:
            return daySeries(metric, day: range.lowerBound, cal: cal)
        case .week, .month:
            return range.enumerated().map { i, day in
                let d = summary(day)
                var b = Bucket(index: i, date: cal.date(day), value: metric.value(d, profile: profile))
                if metric == .heartRate, let lo = d.hrMin, let hi = d.hrMax {
                    b.low = Double(lo)
                    b.high = Double(hi)
                }
                return b
            }
        case .year:
            var out: [Bucket] = []
            var s = range.lowerBound
            for i in 0..<12 {
                let e = cal.addMonths(s, 1) - 1
                let vals = (s...e).compactMap { metric.value(summary($0), profile: profile) }
                let v: Double? = vals.isEmpty ? nil : vals.reduce(0, +) / Double(vals.count)
                out.append(Bucket(index: i, date: cal.date(s), value: v,
                                  low: vals.min(), high: vals.max()))
                s = e + 1
            }
            return out
        }
    }

    private func daySeries(_ metric: Metric, day: Int, cal: DayCalendar) -> [Bucket] {
        let d = summary(day)
        let start = cal.date(day)
        func hourly(_ a: [Int]) -> [Bucket] {
            a.enumerated().map { h, v in
                Bucket(index: h, date: start.addingTimeInterval(Double(h) * 3600),
                       value: d.hasData ? Double(v) : nil)
            }
        }
        switch metric {
        case .steps: return hourly(d.stepsHourly)
        case .distance:
            let f = profile.strideMetres / 1000 / (profile.useMetric ? 1 : 1.609344)
            return hourly(d.stepsHourly).map { var b = $0; b.value = b.value.map { $0 * f }; return b }
        case .calories: return hourly(d.caloriesHourly)
        case .activeMinutes: return hourly(d.activeHourly)
        case .water: return hourly(d.waterHourly)
        case .heartRate, .restingHR, .zoneMinutes:
            return d.heart.enumerated().map { i, p in
                Bucket(index: i, date: start.addingTimeInterval(Double(p.second)),
                       value: Double(p.avg), low: Double(p.avg), high: Double(p.max))
            }
        case .battery:
            return d.battery.enumerated().map { i, p in
                Bucket(index: i, date: start.addingTimeInterval(Double(p.second)), value: Double(p.pct))
            }
        case .workouts:
            return d.workouts.enumerated().map { i, w in
                Bucket(index: i, date: cal.date(watchTime: w.start), value: Double(w.activeSeconds) / 60)
            }
        case .sleep, .sleepScore:
            guard let s = d.mainSleep else { return [] }
            let bed = cal.date(watchTime: s.bed)
            return s.movement.enumerated().map { i, mv in
                Bucket(index: i, date: bed.addingTimeInterval(Double(i) * 300), value: Double(mv))
            }
        }
    }

    public func stats(_ metric: Metric, span: Span, page: Int, cal: DayCalendar) -> PeriodStats {
        let r = period(span, page: page, cal: cal)
        let p = period(span, page: page + 1, cal: cal)
        var st = PeriodStats()
        let lastDay = min(r.upperBound, today)
        let vals: [(Int, Double)] = r.lowerBound <= lastDay
            ? (r.lowerBound...lastDay).compactMap { day in
                metric.value(summary(day), profile: profile).map { (day, $0) }
            } : []
        st.daysWithData = vals.count
        if !vals.isEmpty {
            st.average = vals.map(\.1).reduce(0, +) / Double(vals.count)
            if metric.isTotal { st.total = vals.map(\.1).reduce(0, +) }
            let best = metric.lowerIsBetter ? vals.min { $0.1 < $1.1 } : vals.max { $0.1 < $1.1 }
            st.best = best?.1
            st.bestDay = best?.0
            if let g = metric.goal(profile) { st.goalDays = vals.filter { $0.1 >= g }.count }
        }
        let pv = p.compactMap { metric.value(summary($0), profile: profile) }
        if !pv.isEmpty { st.previousAverage = pv.reduce(0, +) / Double(pv.count) }
        return st
    }

    /// consecutive days meeting the goal; today counts once it is met, and
    /// an unfinished today does not break the run
    public func streak(_ metric: Metric) -> Streak {
        guard let goal = metric.goal(profile), let first = firstDay, first <= today else { return Streak() }
        var s = Streak()
        var run = 0
        for day in first...today {
            let met = (metric.value(summary(day), profile: profile) ?? 0) >= goal
            if met {
                run += 1
                if run > s.longest {
                    s.longest = run
                    s.longestEnd = day
                }
            } else if day != today {
                run = 0
            }
        }
        s.current = run
        return s
    }

    public func personalBests(cal: DayCalendar) -> [PersonalBest] {
        var out: [PersonalBest] = []
        let all = days.values.filter { $0.hasData }
        func best(_ title: String, _ m: Metric) {
            let vals = all.compactMap { d in m.value(d, profile: profile).map { (d.day, $0) } }
                .filter { $0.1 > 0 }
            guard let b = m.lowerIsBetter ? vals.min(by: { $0.1 < $1.1 }) : vals.max(by: { $0.1 < $1.1 })
            else { return }
            out.append(PersonalBest(title: title, value: m.format(b.1, profile: profile), day: b.0))
        }
        best("Most steps in a day", .steps)
        best("Longest distance", .distance)
        best("Most active minutes", .activeMinutes)
        best("Most heart zone minutes", .zoneMinutes)
        best("Lowest resting heart rate", .restingHR)
        best("Longest sleep", .sleep)
        best("Best sleep score", .sleepScore)
        best("Most water", .water)
        let ws = all.flatMap(\.workouts)
        if let w = ws.max(by: { $0.activeSeconds < $1.activeSeconds }) {
            out.append(PersonalBest(title: "Longest workout", value: Metric.hm(w.activeSeconds / 60), day: w.day))
        }
        if let w = ws.max(by: { $0.sets < $1.sets }), w.sets > 0 {
            out.append(PersonalBest(title: "Most sets in a workout", value: "\(w.sets) sets", day: w.day))
        }
        if let w = ws.max(by: { $0.maxBPM < $1.maxBPM }), w.maxBPM > 0 {
            out.append(PersonalBest(title: "Highest workout heart rate", value: "\(w.maxBPM) bpm", day: w.day))
        }
        let st = streak(.steps)
        if st.longest > 1 {
            out.append(PersonalBest(title: "Longest step goal streak", value: "\(st.longest) days", day: st.longestEnd))
        }
        return out
    }

    /// sleep regularity over the last n nights: how much bedtime and wake
    /// time wander (standard deviation, minutes) and a 0-100 score
    public func sleepConsistency(nights n: Int = 14) -> (bedSD: Int, wakeSD: Int, score: Int, nights: Int)? {
        let sessions = days.values.compactMap(\.mainSleep)
            .filter { $0.wakeDay > today - n && $0.wakeDay <= today }
        guard sessions.count >= 3 else { return nil }
        func sd(_ v: [Int]) -> Double {
            let m = Double(v.reduce(0, +)) / Double(v.count)
            return (v.map { pow(Double($0) - m, 2) }.reduce(0, +) / Double(v.count)).squareRoot()
        }
        let b = sd(sessions.map(\.bedMinute)), w = sd(sessions.map(\.wakeMinute))
        let score = max(0, min(100, Int((100 - (b + w) / 2 * 100 / 90).rounded())))
        return (Int(b.rounded()), Int(w.rounded()), score, sessions.count)
    }

    /// short observations for the Today screen, only where the data shows them
    public func insights(cal: DayCalendar) -> [String] {
        var out: [String] = []
        func avg(_ m: Metric, _ r: ClosedRange<Int>) -> Double? {
            let v = r.compactMap { m.value(summary($0), profile: profile) }
            return v.count >= 3 ? v.reduce(0, +) / Double(v.count) : nil
        }
        let last7 = (today - 7)...(today - 1), prev7 = (today - 14)...(today - 8)
        if let a = avg(.steps, last7), let b = avg(.steps, prev7), b > 0 {
            let c = Int(((a - b) / b * 100).rounded())
            if abs(c) >= 5 {
                out.append("Your daily steps averaged \(Metric.steps.format(a, profile: profile)) this past week, \(abs(c))% \(c > 0 ? "more" : "fewer") than the week before.")
            }
        }
        if let a = avg(.restingHR, last7), let b = avg(.restingHR, (today - 37)...(today - 8)) {
            let c = Int((a - b).rounded())
            if abs(c) >= 3 {
                out.append("Your resting heart rate is \(abs(c)) bpm \(c > 0 ? "above" : "below") your 30-day average.")
            }
        }
        if let a = avg(.sleep, last7) {
            let g = Double(profile.sleepGoalMinutes)
            if a < g - 30 {
                out.append("You slept \(Metric.hm(Int(a))) a night on average this week, \(Metric.hm(Int(g - a))) under your goal.")
            }
        }
        let s = streak(.steps)
        if s.current >= 3 {
            out.append("\(s.current) days in a row at your step goal.")
        }
        return out
    }
}
