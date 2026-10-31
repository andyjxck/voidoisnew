import SwiftUI
import VoidHealthCore

/// the day at a glance: goal rings, every metric as a tile, and the day's
/// steps, heart rate, water and workouts. Pages back through earlier days.
struct TodayView: View {
    @Environment(AppModel.self) private var app
    @Environment(\.scenePhase) private var phase
    @State private var page = 0
    @State private var celebrating: GoalHit?

    var body: some View {
        let m = app.model
        let day = m.today - page
        let d = m.summary(day)
        let p = m.profile
        let isToday = page == 0
        Screen {
            SyncBar()
            if let e = app.storeError ?? app.link.lastError {
                Label(e, systemImage: "exclamationmark.triangle").font(.footnote).foregroundStyle(.orange)
            }
            PeriodHeader(title: Fmt.dayTitle(day, today: m.today, cal: app.cal),
                         subtitle: isToday ? app.cal.date(day).formatted(.dateTime.weekday(.wide).day().month(.wide)) : nil,
                         canBack: m.firstDay.map { day > $0 } ?? false,
                         canForward: page > 0,
                         back: { page += 1 },
                         forward: { page = max(0, page - 1) })

            if !d.hasData {
                Card {
                    EmptyNote(text: emptyText(isToday: isToday), icon: "applewatch")
                }
            }

            GoalRings(d: d, profile: p)

            if isToday && m.firstDay == nil {
                FirstRunCard()
            }

            tiles(d, p, day: day, isToday: isToday)

            Card {
                CardTitle(title: "Steps by hour", icon: Metric.steps.icon, color: Metric.steps.color,
                          trailing: d.stepsHourly.max().map { $0 > 0 ? "Peak \(Fmt.number($0))" : "" } ?? "")
                HourlyBars(values: d.stepsHourly, day: app.cal.date(day), color: Metric.steps.color)
            }

            heartCard(d, p, day: day, isToday: isToday)

            if d.water > 0 || isToday {
                waterCard(d, p, day: day)
            }

            if !d.workouts.isEmpty {
                Card {
                    CardTitle(title: "Workouts", icon: Metric.workouts.icon, color: Metric.workouts.color,
                              trailing: Metric.hm(d.workoutMinutes))
                    ForEach(d.workouts) { w in
                        NavigationLink(value: w) {
                            WorkoutRow(w: w, cal: app.cal)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }

            if isToday {
                let ins = m.insights(cal: app.cal)
                if !ins.isEmpty {
                    Card {
                        CardTitle(title: "Insights", icon: "sparkles", color: .yellow)
                        ForEach(ins, id: \.self) { t in
                            Text(t).font(.subheadline)
                        }
                    }
                }
            }

            if d.clockGuess {
                Text("Some of this day was logged before the watch clock was set, so times may be off.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .navigationTitle(isToday ? "Today" : Fmt.shortDay(day, cal: app.cal))
        .navigationBarTitleDisplayMode(.large)
        .navigationDestination(for: Metric.self) { metric in
            MetricDetailView(metric: metric, day: metric.spans.contains(.day) ? day : nil)
        }
        .navigationDestination(for: Workout.self) { w in
            WorkoutDetailView(workout: w)
        }
        .refreshable { await pullToRefresh() }
        .overlay(alignment: .top) {
            if let hit = celebrating {
                GoalCelebration(hit: hit) {
                    celebrating = nil
                    nextCelebration()
                }
                .id(hit)
            }
        }
        .onAppear { nextCelebration() }
        .onChange(of: app.goalQueue) { nextCelebration() }
        .onChange(of: phase) { nextCelebration() }
    }

    /// play the next goal met today, once Today is on screen
    private func nextCelebration() {
        guard celebrating == nil, phase == .active, let hit = app.takeCelebration() else { return }
        celebrating = hit
        GoalNotifier.requestIfNeeded()    // asks once, at a moment that explains it
    }

    /// keeps the spinner up briefly while the watch answers
    private func pullToRefresh() async {
        app.link.refresh()
        try? await Task.sleep(for: .seconds(1.5))
    }

    private func emptyText(isToday: Bool) -> String {
        if isToday {
            return app.link.hasWatch ? "Nothing from your watch yet today. Data arrives every few minutes while it's in range."
                : "Wear your PineTime with VoidOS 2.3 or newer near your iPhone to start syncing."
        }
        return "Your watch logged nothing on this day."
    }

    @ViewBuilder
    private func tiles(_ d: DaySummary, _ p: Profile, day: Int, isToday: Bool) -> some View {
        let cols = [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)]
        LazyVGrid(columns: cols, spacing: 12) {
            tile(.steps, Fmt.number(d.steps), detail: "Goal \(Fmt.number(p.stepGoal))",
                 progress: Double(d.steps) / Double(max(1, p.stepGoal)))
            tile(.distance, Metric.distance.text(Metric.distance.value(d, profile: p), p))
            tile(.calories, "\(Fmt.number(d.caloriesTotal)) kcal", detail: "\(Fmt.number(d.caloriesActive)) active",
                 progress: p.calorieGoal > 0 ? Double(d.caloriesTotal) / Double(p.calorieGoal) : nil)
            tile(.activeMinutes, "\(d.activeMinutes) min", detail: "Goal \(p.activeMinuteGoal) min",
                 progress: Double(d.activeMinutes) / Double(max(1, p.activeMinuteGoal)))
            heartTile(d, isToday: isToday)
            tile(.restingHR, d.restingHR.map { "\($0) bpm" } ?? "--",
                 detail: d.restingHR == nil ? "Needs a night of wear" : d.restingEstimated ? "Estimated" : nil)
            tile(.zoneMinutes, "\(d.zoneTotal) min", detail: "\(d.cardioMinutes) min cardio or peak")
            sleepTile(d, p)
            tile(.water, Metric.water.format(Double(d.water), profile: p), detail: "Goal \(Metric.water.format(Double(p.waterGoalML), profile: p))",
                 progress: Double(d.water) / Double(max(1, p.waterGoalML)))
            tile(.workouts, d.workouts.isEmpty ? "None" : Metric.hm(d.workoutMinutes),
                 detail: d.workouts.isEmpty ? nil : "\(d.workouts.count) workout\(d.workouts.count == 1 ? "" : "s")")
            batteryTile(d, isToday: isToday)
            NavigationLink(value: Metric.sleepScore) {
                StatTile(metric: .sleepScore, value: d.mainSleep.map { "\($0.score)" } ?? "--",
                         detail: d.mainSleep.map { "\($0.stillPercent)% still" })
            }
            .buttonStyle(.plain)
        }
    }

    private func tile(_ metric: Metric, _ value: String, detail: String? = nil, progress: Double? = nil) -> some View {
        NavigationLink(value: metric) {
            StatTile(metric: metric, value: value, detail: detail, progress: progress)
        }
        .buttonStyle(.plain)
    }

    private func heartTile(_ d: DaySummary, isToday: Bool) -> some View {
        let live = isToday ? app.link.liveBPM : nil
        let value = live.map { "\($0) bpm" } ?? d.hrAvg.map { "\($0) bpm avg" } ?? "--"
        var detail: String?
        if live != nil {
            detail = "Live from your watch"
        } else if let lo = d.hrMin, let hi = d.hrMax {
            detail = "\(lo)–\(hi) bpm"
        }
        return tile(.heartRate, value, detail: detail)
    }

    private func sleepTile(_ d: DaySummary, _ p: Profile) -> some View {
        let s = d.mainSleep
        return tile(.sleep, s.map { Metric.hm($0.minutes) } ?? "--",
                    detail: s.map { "\(Fmt.time(app.cal.date(watchTime: $0.bed))) – \(Fmt.time(app.cal.date(watchTime: $0.wake)))" },
                    progress: s.map { Double($0.minutes) / Double(max(1, p.sleepGoalMinutes)) })
    }

    private func batteryTile(_ d: DaySummary, isToday: Bool) -> some View {
        let now = isToday ? app.link.battery : nil
        let last = d.battery.last
        let pct = now ?? last?.pct
        var detail: String?
        if last?.charging == true && (isToday || now == nil) {
            detail = "Charging"
        } else if let lo = d.battery.map(\.pct).min() {
            detail = "Low of \(lo)% \(isToday ? "today" : "that day")"
        }
        return tile(.battery, pct.map { "\($0)%" } ?? "--", detail: detail,
                    progress: pct.map { Double($0) / 100 })
    }

    private func heartCard(_ d: DaySummary, _ p: Profile, day: Int, isToday: Bool) -> some View {
        Card {
            CardTitle(title: "Heart rate", icon: Metric.heartRate.icon, color: Metric.heartRate.color,
                      trailing: d.restingHR.map { "Resting \($0) bpm" })
            if d.heart.isEmpty {
                EmptyNote(text: "No heart readings for this day.", icon: "heart")
            } else {
                HeartDayChart(points: d.heart, day: app.cal.date(day), restingHR: d.restingHR)
                StatGrid(cells: [
                    ("Average", d.hrAvg.map { "\($0) bpm" } ?? "--", nil),
                    ("Lowest", d.hrMin.map { "\($0) bpm" } ?? "--", nil),
                    ("Highest", d.hrMax.map { "\($0) bpm" } ?? "--", nil),
                ])
                if d.zoneTotal > 0 {
                    Divider()
                    ZoneBreakdown(minutes: d.zoneMinutes, maxHR: p.maxHR)
                }
            }
        }
    }

    private func waterCard(_ d: DaySummary, _ p: Profile, day: Int) -> some View {
        Card {
            CardTitle(title: "Water", icon: Metric.water.icon, color: Metric.water.color,
                      trailing: "\(Metric.water.format(Double(d.water), profile: p)) of \(Metric.water.format(Double(p.waterGoalML), profile: p))")
            ProgressBar(progress: Double(d.water) / Double(max(1, p.waterGoalML)), color: Metric.water.color)
            if d.waterEvents.isEmpty {
                Text("Log water with the water tile on your watch.").font(.caption).foregroundStyle(.secondary)
            } else {
                let start = app.cal.date(day)
                ForEach(d.waterEvents.indices.reversed(), id: \.self) { i in
                    let e = d.waterEvents[i]
                    HStack {
                        Text(Fmt.time(start.addingTimeInterval(Double(e.second))))
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(e.delta > 0 ? "+\(e.delta) ml" : "\(e.delta) ml")
                            .foregroundStyle(e.delta > 0 ? Color.primary : Color.orange)
                    }
                    .font(.subheadline)
                    .monospacedDigit()
                }
            }
        }
    }
}

/// steps, active minutes and water as three rings, Google Health style
struct GoalRings: View {
    let d: DaySummary
    let profile: Profile

    var body: some View {
        let p = profile
        let rings: [(Metric, Double, String, String)] = [
            (.steps, Double(d.steps) / Double(max(1, p.stepGoal)), Fmt.number(d.steps), "of \(Fmt.number(p.stepGoal)) steps"),
            (.activeMinutes, Double(d.activeMinutes) / Double(max(1, p.activeMinuteGoal)), "\(d.activeMinutes)",
             "of \(p.activeMinuteGoal) active min"),
            (.water, Double(d.water) / Double(max(1, p.waterGoalML)), Metric.water.format(Double(d.water), profile: p),
             "of \(Metric.water.format(Double(p.waterGoalML), profile: p)) water"),
        ]
        Card {
            HStack(spacing: 20) {
                ZStack {
                    ForEach(rings.indices, id: \.self) { i in
                        ProgressRing(progress: rings[i].1, color: rings[i].0.color, lineWidth: 13)
                            .padding(CGFloat(i) * 17)
                    }
                    Image(systemName: "figure.walk").font(.title3).foregroundStyle(.secondary)
                }
                .frame(width: 140, height: 140)
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(rings.indices, id: \.self) { i in
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Circle().fill(rings[i].0.color).frame(width: 8, height: 8)
                            VStack(alignment: .leading, spacing: 0) {
                                Text(rings[i].2).font(.title3.weight(.semibold)).monospacedDigit()
                                Text(rings[i].3).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                Spacer(minLength: 0)
            }
        }
    }
}

struct FirstRunCard: View {
    var body: some View {
        Card {
            CardTitle(title: "Getting started", icon: "applewatch", color: .teal)
            VStack(alignment: .leading, spacing: 6) {
                Text("1. Put VoidOS 2.3 or newer on your PineTime.")
                Text("2. Open this app with the watch nearby. If iOS asks to pair, accept.")
                Text("3. Keep this app installed. It finds the watch and syncs on its own, even in the background.")
                Text("Set your age, height, weight and goals in the You tab so calories, distance and heart zones fit you.")
                    .foregroundStyle(.secondary)
            }
            .font(.subheadline)
        }
    }
}
