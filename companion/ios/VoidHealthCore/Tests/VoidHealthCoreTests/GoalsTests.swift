import XCTest
@testable import VoidHealthCore

final class GoalsTests: XCTestCase {
    private func summary(_ day: Int, steps: Int = 0, active: Int = 0, water: Int = 0) -> DaySummary {
        var s = DaySummary(day: day)
        s.steps = steps
        s.activeMinutes = active
        s.water = water
        return s
    }

    func testEachGoalOncePerDay() {
        var p = Profile()
        p.stepGoal = 8000
        p.activeMinuteGoal = 30
        p.waterGoalML = 2000
        var c = GoalCelebrations()
        let d = 20261031

        XCTAssertEqual(c.take(today: d, summary: summary(d, steps: 7999), profile: p), [])
        XCTAssertEqual(c.take(today: d, summary: summary(d, steps: 8000), profile: p),
                       [GoalHit(goal: .steps, day: d, target: 8000)])
        // already celebrated: more steps, a higher goal, a relaunch
        XCTAssertEqual(c.take(today: d, summary: summary(d, steps: 12000), profile: p), [])
        let saved = try! JSONDecoder().decode(GoalCelebrations.self, from: JSONEncoder().encode(c))
        var c2 = saved
        XCTAssertEqual(c2.take(today: d, summary: summary(d, steps: 12000), profile: p), [])

        // two goals closing in the same update come back in ring order
        XCTAssertEqual(c.take(today: d, summary: summary(d, steps: 12000, active: 30, water: 2000), profile: p).map(\.goal),
                       [.activeMinutes, .water])

        // the next day starts over
        XCTAssertEqual(c.take(today: d + 1, summary: summary(d + 1, steps: 9000), profile: p).map(\.goal), [.steps])
    }

    func testNothingForAnotherDayOrAClockStepBack() {
        var p = Profile()
        p.stepGoal = 1000
        var c = GoalCelebrations()
        // a summary of yesterday doesn't celebrate today
        XCTAssertEqual(c.take(today: 20261101, summary: summary(20261031, steps: 5000), profile: p), [])
        XCTAssertEqual(c.take(today: 20261101, summary: summary(20261101, steps: 5000), profile: p).count, 1)
        // a time zone change back to the 31st doesn't re-arm anything
        XCTAssertEqual(c.take(today: 20261031, summary: summary(20261031, steps: 5000), profile: p), [])
        XCTAssertEqual(c.take(today: 20261101, summary: summary(20261101, steps: 9000), profile: p), [])
    }

    func testZeroGoalNeverCelebrates() {
        var p = Profile()
        p.waterGoalML = 0
        var c = GoalCelebrations()
        XCTAssertFalse(c.take(today: 1, summary: summary(1, water: 0), profile: p).contains { $0.goal == .water })
    }
}
