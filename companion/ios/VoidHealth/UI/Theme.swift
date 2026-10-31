import SwiftUI
import VoidHealthCore

extension Metric {
    var color: Color {
        switch self {
        case .steps: return .teal
        case .distance: return .blue
        case .calories: return .orange
        case .activeMinutes: return .green
        case .heartRate: return .red
        case .restingHR: return .pink
        case .zoneMinutes: return .yellow
        case .sleep, .sleepScore: return .indigo
        case .water: return .cyan
        case .workouts: return .mint
        case .battery: return .gray
        }
    }

    var icon: String {
        switch self {
        case .steps: return "figure.walk"
        case .distance: return "map.fill"
        case .calories: return "flame.fill"
        case .activeMinutes: return "figure.run"
        case .heartRate: return "heart.fill"
        case .restingHR: return "heart.circle.fill"
        case .zoneMinutes: return "bolt.heart.fill"
        case .sleep: return "bed.double.fill"
        case .sleepScore: return "moon.stars.fill"
        case .water: return "drop.fill"
        case .workouts: return "dumbbell.fill"
        case .battery: return "battery.75percent"
        }
    }

    /// drawn as bars (totals) or as a line (levels)
    var usesBars: Bool { isTotal || self == .sleep }

    /// what the number of a multi-day period means
    var averageLabel: String {
        switch self {
        case .heartRate: return "Average heart rate"
        case .restingHR: return "Average resting"
        case .sleepScore: return "Average score"
        case .battery: return "Average daily low"
        default: return "Daily average"
        }
    }

    func text(_ v: Double?, _ p: Profile) -> String {
        guard let v else { return "--" }
        return format(v, profile: p)
    }
}

extension HRZone {
    var color: Color {
        switch self {
        case .below: return .gray
        case .fatBurn: return .yellow
        case .cardio: return .orange
        case .peak: return .red
        }
    }
}

enum Fmt {
    static func number(_ n: Int) -> String {
        NumberFormatter.localizedString(from: NSNumber(value: n), number: .decimal)
    }

    static func time(_ d: Date) -> String {
        d.formatted(date: .omitted, time: .shortened)
    }

    static func dayTitle(_ day: Int, today: Int, cal: DayCalendar) -> String {
        if day == today { return "Today" }
        if day == today - 1 { return "Yesterday" }
        return cal.date(day).formatted(.dateTime.weekday(.wide).day().month(.abbreviated))
    }

    static func shortDay(_ day: Int, cal: DayCalendar) -> String {
        cal.date(day).formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated))
    }

    static func periodTitle(_ span: Span, range: ClosedRange<Int>, today: Int, cal: DayCalendar) -> String {
        let a = cal.date(range.lowerBound), b = cal.date(range.upperBound)
        switch span {
        case .day:
            return dayTitle(range.lowerBound, today: today, cal: cal)
        case .week:
            if range.contains(today) { return "This week" }
            return "\(a.formatted(.dateTime.day().month(.abbreviated))) – \(b.formatted(.dateTime.day().month(.abbreviated)))"
        case .month:
            return a.formatted(.dateTime.month(.wide).year())
        case .year:
            return "\(a.formatted(.dateTime.month(.abbreviated).year())) – \(b.formatted(.dateTime.month(.abbreviated).year()))"
        }
    }

    /// minutes after 18:00 to a clock time
    static func clock(minutesAfterSix m: Int) -> String {
        clock(minuteOfDay: (m + 18 * 60) % 1440)
    }

    static func clock(minuteOfDay m: Int) -> String {
        let mm = ((m % 1440) + 1440) % 1440
        let d = Calendar.current.date(bySettingHour: mm / 60, minute: mm % 60, second: 0, of: Date()) ?? Date()
        return time(d)
    }

    static func height(_ cm: Double, metric: Bool) -> String {
        if metric { return "\(Int(cm.rounded())) cm" }
        let inches = Int((cm / 2.54).rounded())
        return "\(inches / 12)′ \(inches % 12)″"
    }

    static func weight(_ kg: Double, metric: Bool) -> String {
        metric ? String(format: "%.1f kg", kg) : String(format: "%.0f lb", kg * 2.20462)
    }
}
