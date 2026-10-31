import Foundation

/// The daily goals the app celebrates when their ring closes on Today.
public enum DailyGoal: String, CaseIterable, Codable, Sendable {
    case steps, activeMinutes, water

    public var metric: Metric {
        switch self {
        case .steps: return .steps
        case .activeMinutes: return .activeMinutes
        case .water: return .water
        }
    }

    public func value(_ d: DaySummary) -> Int {
        switch self {
        case .steps: return d.steps
        case .activeMinutes: return d.activeMinutes
        case .water: return d.water
        }
    }

    public func target(_ p: Profile) -> Int {
        switch self {
        case .steps: return p.stepGoal
        case .activeMinutes: return p.activeMinuteGoal
        case .water: return p.waterGoalML
        }
    }

    public var title: String {
        switch self {
        case .steps: return "Step goal reached"
        case .activeMinutes: return "Active minutes goal reached"
        case .water: return "Water goal reached"
        }
    }
}

/// One goal met on one day, waiting to be shown.
public struct GoalHit: Codable, Equatable, Hashable, Sendable {
    public var goal: DailyGoal
    public var day: Int
    /// the goal it reached, as set at the time
    public var target: Int

    public init(goal: DailyGoal, day: Int, target: Int) {
        self.goal = goal
        self.day = day
        self.target = target
    }
}

/// Which goals have been celebrated today, so each goal is celebrated once
/// a day. The app saves it, so a relaunch doesn't celebrate again.
public struct GoalCelebrations: Codable, Equatable, Sendable {
    public private(set) var day: Int?
    public private(set) var done: Set<DailyGoal> = []

    public init() {}

    /// The goals `summary` meets today that haven't been celebrated yet,
    /// marked as celebrated. Nothing for a summary of another day, and a
    /// clock that steps back a day (time zone change) never re-arms one.
    public mutating func take(today: Int, summary: DaySummary, profile: Profile) -> [GoalHit] {
        if let d = day, today < d { return [] }
        if day != today {
            day = today
            done = []
        }
        guard summary.day == today else { return [] }
        var hits: [GoalHit] = []
        for g in DailyGoal.allCases where !done.contains(g) {
            let t = g.target(profile)
            if t > 0 && g.value(summary) >= t {
                done.insert(g)
                hits.append(GoalHit(goal: g, day: today, target: t))
            }
        }
        return hits
    }
}
