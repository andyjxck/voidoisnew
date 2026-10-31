import SwiftUI
import VoidHealthCore

/// last night (or any night): score, timing, movement and heart rate,
/// then the week, consistency and recent nights
struct SleepView: View {
    @Environment(AppModel.self) private var app
    @State private var page = 0

    var body: some View {
        let m = app.model
        let p = m.profile
        let day = m.today - page
        let d = m.summary(day)
        Screen {
            PeriodHeader(title: nightTitle(day, today: m.today),
                         subtitle: "Woke \(Fmt.dayTitle(day, today: m.today, cal: app.cal).lowercased())",
                         canBack: m.firstDay.map { day > $0 } ?? false,
                         canForward: page > 0,
                         back: { page += 1 },
                         forward: { page = max(0, page - 1) })

            if let s = d.mainSleep {
                NightCard(s: s, profile: p)
                Card {
                    CardTitle(title: "Through the night", icon: "waveform.path", color: .indigo)
                    SleepNightChart(session: s, bed: app.cal.date(watchTime: s.bed))
                }
                Card {
                    CardTitle(title: "Details")
                    StatGrid(cells: [
                        ("In bed", Metric.hm(s.minutes), nil),
                        ("Still", "\(s.stillPercent)%", nil),
                        ("Restless", Metric.hm(s.restlessMinutes), nil),
                        ("Average HR", s.avgBPM.map { "\($0) bpm" } ?? "--", nil),
                        ("Lowest HR", s.lowBPM.map { "\($0) bpm" } ?? "--", nil),
                        ("Goal", Metric.hm(p.sleepGoalMinutes), goalNote(s, p)),
                    ])
                }
                let naps = d.sleep.filter { $0.id != s.id }
                if !naps.isEmpty {
                    Card {
                        CardTitle(title: "Other sleep this day", icon: "zzz", color: .indigo)
                        ForEach(naps) { n in
                            HStack {
                                Text("\(Fmt.time(app.cal.date(watchTime: n.bed))) – \(Fmt.time(app.cal.date(watchTime: n.wake)))")
                                Spacer()
                                Text(Metric.hm(n.minutes)).monospacedDigit()
                            }
                            .font(.subheadline)
                        }
                    }
                }
            } else {
                Card {
                    EmptyNote(text: page == 0 ? "No sleep logged last night. Wear the watch to bed and it records your sleep automatically."
                                              : "No sleep logged for this night.", icon: "bed.double")
                }
            }

            weekCard(m, p)
            consistencyCard(m)
            recentCard(m)

            Text("The sleep score is out of 100: up to 50 for time asleep against your goal, 25 for how still you were, and 25 for going to bed near your average bedtime over the previous week.")
                .font(.footnote).foregroundStyle(.secondary).padding(.horizontal, 4)
        }
        .navigationTitle("Sleep")
        .navigationDestination(for: Metric.self) { metric in
            MetricDetailView(metric: metric)
        }
    }

    private func nightTitle(_ day: Int, today: Int) -> String {
        if day == today { return "Last night" }
        return "Night of " + app.cal.date(day - 1).formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated))
    }

    private func goalNote(_ s: SleepSession, _ p: Profile) -> String {
        let diff = s.minutes - p.sleepGoalMinutes
        return diff >= 0 ? "met" : "\(Metric.hm(-diff)) short"
    }

    private func weekCard(_ m: HealthModel, _ p: Profile) -> some View {
        let range = m.period(.week, page: 0, cal: app.cal)
        let st = m.stats(.sleep, span: .week, page: 0, cal: app.cal)
        return Card {
            NavigationLink(value: Metric.sleep) {
                HStack {
                    CardTitle(title: "This week", icon: Metric.sleep.icon, color: .indigo,
                              trailing: st.average.map { "avg " + Metric.hm(Int($0)) })
                    Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
                }
            }
            .buttonStyle(.plain)
            MetricChart(metric: .sleep, span: .week, buckets: m.series(.sleep, span: .week, page: 0, cal: app.cal),
                        domain: app.cal.date(range.lowerBound)...app.cal.date(range.upperBound + 1),
                        profile: p, goal: Double(p.sleepGoalMinutes), height: 160)
            HStack {
                Text("Goal met \(st.goalDays) of \(st.daysWithData) nights")
                Spacer()
                NavigationLink(value: Metric.sleepScore) {
                    Text("Score trend")
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func consistencyCard(_ m: HealthModel) -> some View {
        if let c = m.sleepConsistency() {
            let sessions = (0..<14).compactMap { m.summary(m.today - $0).mainSleep }
            let bed = sessions.isEmpty ? nil : sessions.map(\.bedMinute).reduce(0, +) / sessions.count
            let wake = sessions.isEmpty ? nil : sessions.map(\.wakeMinute).reduce(0, +) / sessions.count
            Card {
                CardTitle(title: "Consistency", icon: "clock.arrow.circlepath", color: .indigo,
                          trailing: "last \(c.nights) nights")
                HStack(alignment: .center, spacing: 18) {
                    ZStack {
                        ProgressRing(progress: Double(c.score) / 100, color: .indigo, lineWidth: 8)
                        Text("\(c.score)").font(.title3.weight(.semibold)).monospacedDigit()
                    }
                    .frame(width: 70, height: 70)
                    StatGrid(cells: [
                        ("Usual bedtime", bed.map { Fmt.clock(minutesAfterSix: $0) } ?? "--", "±\(c.bedSD) min"),
                        ("Usual wake", wake.map { Fmt.clock(minuteOfDay: $0) } ?? "--", "±\(c.wakeSD) min"),
                    ])
                }
            }
        }
    }

    @ViewBuilder
    private func recentCard(_ m: HealthModel) -> some View {
        let nights = (0..<14).compactMap { i -> (Int, SleepSession)? in
            guard let s = m.summary(m.today - i).mainSleep else { return nil }
            return (m.today - i, s)
        }
        if !nights.isEmpty {
            Card {
                CardTitle(title: "Recent nights")
                ForEach(nights, id: \.0) { n in
                    Button {
                        page = m.today - n.0
                    } label: {
                        HStack {
                            Text(Fmt.shortDay(n.0, cal: app.cal)).frame(width: 110, alignment: .leading)
                            ProgressBar(progress: Double(n.1.minutes) / Double(max(1, m.profile.sleepGoalMinutes)),
                                        color: .indigo)
                            Text(Metric.hm(n.1.minutes)).monospacedDigit().frame(width: 62, alignment: .trailing)
                            Text("\(n.1.score)").monospacedDigit().foregroundStyle(.secondary)
                                .frame(width: 28, alignment: .trailing)
                        }
                        .font(.subheadline)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }
}

/// score ring with the night's headline numbers
struct NightCard: View {
    @Environment(AppModel.self) private var app
    let s: SleepSession
    let profile: Profile

    var body: some View {
        Card {
            HStack(spacing: 20) {
                ZStack {
                    ProgressRing(progress: Double(s.score) / 100, color: .indigo, lineWidth: 12)
                    VStack(spacing: 0) {
                        Text("\(s.score)").font(.largeTitle.weight(.bold)).monospacedDigit()
                        Text(scoreWord).font(.caption).foregroundStyle(.secondary)
                    }
                }
                .frame(width: 120, height: 120)
                VStack(alignment: .leading, spacing: 10) {
                    VStack(alignment: .leading, spacing: 0) {
                        Text(Metric.hm(s.minutes)).font(.title2.weight(.semibold)).monospacedDigit()
                        Text("asleep").font(.caption).foregroundStyle(.secondary)
                    }
                    VStack(alignment: .leading, spacing: 0) {
                        Text("\(Fmt.time(app.cal.date(watchTime: s.bed))) – \(Fmt.time(app.cal.date(watchTime: s.wake)))")
                            .font(.subheadline.weight(.medium)).monospacedDigit()
                        Text("bedtime to wake").font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }
        }
    }

    private var scoreWord: String {
        switch s.score {
        case 90...: return "Excellent"
        case 80..<90: return "Good"
        case 60..<80: return "Fair"
        default: return "Poor"
        }
    }
}
