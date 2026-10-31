import Foundation

/// What the estimates need about the wearer, and the daily goals.
public struct Profile: Codable, Equatable, Sendable {
    public enum Sex: String, Codable, CaseIterable, Sendable { case male, female, other }

    public var age: Int = 30
    public var sex: Sex = .other
    public var heightCm: Double = 175
    public var weightKg: Double = 75
    /// 0 = estimate as 220 - age
    public var maxHROverride: Int = 0

    public var stepGoal: Int = 10_000
    public var activeMinuteGoal: Int = 30
    public var waterGoalML: Int = 2_000
    public var sleepGoalMinutes: Int = 480
    public var calorieGoal: Int = 0          // 0 = no goal
    public var useMetric: Bool = true

    public init() {}

    public var maxHR: Int { maxHROverride > 0 ? maxHROverride : max(120, 220 - age) }
    /// metres per step, estimated from height
    public var strideMetres: Double { heightCm / 100 * (sex == .female ? 0.413 : 0.415) }

    /// resting energy per day, Mifflin-St Jeor
    public var bmr: Double {
        let base = 10 * weightKg + 6.25 * heightCm - 5 * Double(age)
        switch sex {
        case .male: return base + 5
        case .female: return base - 161
        case .other: return base - 78
        }
    }
}

/// Heart rate zones as a share of max heart rate.
public enum HRZone: Int, CaseIterable, Codable, Sendable, Comparable {
    case below = 0, fatBurn, cardio, peak

    public static func < (a: HRZone, b: HRZone) -> Bool { a.rawValue < b.rawValue }

    public static func of(bpm: Int, maxHR: Int) -> HRZone {
        let p = Double(bpm) / Double(maxHR)
        if p >= 0.85 { return .peak }
        if p >= 0.70 { return .cardio }
        if p >= 0.50 { return .fatBurn }
        return .below
    }

    public var name: String {
        switch self {
        case .below: return "Below zones"
        case .fatBurn: return "Fat burn"
        case .cardio: return "Cardio"
        case .peak: return "Peak"
        }
    }

    /// bpm range for display
    public func range(maxHR: Int) -> ClosedRange<Int> {
        let m = Double(maxHR)
        switch self {
        case .below: return 0...Int(m * 0.5) - 1
        case .fatBurn: return Int(m * 0.5)...Int(m * 0.7) - 1
        case .cardio: return Int(m * 0.7)...Int(m * 0.85) - 1
        case .peak: return Int(m * 0.85)...maxHR + 40
        }
    }
}
