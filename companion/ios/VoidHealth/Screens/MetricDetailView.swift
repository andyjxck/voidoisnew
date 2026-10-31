import SwiftUI
import VoidHealthCore

/// one metric over a day, week, month or year, with the period's numbers,
/// the change from the period before, goal days and streaks
struct MetricDetailView: View {
    @Environment(AppModel.self) private var app
    let metric: Metric
    @State private var span: Span
    @State private var page: Int
    private let openedDay: Int?

    init(metric: Metric, day: Int? = nil, span: Span? = nil) {
        self.metric = metric
        self.openedDay = day
        let s = span ?? (day != nil && metric.spans.contains(.day) ? .day : .week)
        _span = State(initialValue: metric.spans.contains(s) ? s : .week)
        _page = State(initialValue: 0)
    }

    var body: some View {
        let m = app.model
        let p = m.profile
        let range = m.period(span, page: page, cal: app.cal)
        let buckets = m.series(metric, span: span, page: page, cal: app.cal)
        let st = m.stats(metric, span: span, page: page, cal: app.cal)
        let start = app.cal.date(range.lowerBound)
        let end = app.cal.date(range.upperBound + 1)
        Screen {
            Picker("Span", selection: $span) {
                ForEach(metric.spans) { s in
                    Text(s.label).tag(s)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: span) { _, _ in page = 0 }

            PeriodHeader(title: Fmt.periodTitle(span, range: range, today: m.today, cal: app.cal),
                         canBack: m.firstDay.map { range.lowerBound > $0 } ?? false,
                         canForward: page > 0,
                         back: { page += 1 },
                         forward: { page = max(0, page - 1) })

            Card {
                headline(m, st, range: range)
                MetricChart(metric: metric, span: span, buckets: buckets,
                            domain: start...end, profile: p, goal: metric.goal(p))
                    .id("\(span.rawValue)\(page)")
            }

            if span != .day {
                Card {
                    CardTitle(title: "This period")
                    StatGrid(cells: statCells(st, p, days: range.count))
                }
            } else {
                dayExtras(m.summary(range.lowerBound), p)
            }

            if metric == .heartRate || metric == .zoneMinutes {
                zoneCard(m, range: range, p)
            }

            if metric.goal(p) != nil {
                let s = m.streak(metric)
                Card {
                    CardTitle(title: "Goal streak", icon: "flame", color: .orange)
                    StatGrid(cells: [
                        ("Current", "\(s.current) day\(s.current == 1 ? "" : "s")", nil),
                        ("Longest", "\(s.longest) day\(s.longest == 1 ? "" : "s")",
                         s.longestEnd.map { "ended \(Fmt.shortDay($0, cal: app.cal))" }),
                        ("Goal", metric.text(metric.goal(p), p), nil),
                    ])
                }
            }

            if let about = about(p) {
                Text(about).font(.footnote).foregroundStyle(.secondary).padding(.horizontal, 4)
            }
        }
        .navigationTitle(metric.title)
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func headline(_ m: HealthModel, _ st: PeriodStats, range: ClosedRange<Int>) -> some View {
        let p = m.profile
        if span == .day {
            let v = metric.value(m.summary(range.lowerBound), profile: p)
            VStack(alignment: .leading, spacing: 2) {
                Text(dayLabel).font(.caption).foregroundStyle(.secondary)
                Text(metric.text(v, p)).font(.largeTitle.weight(.semibold)).monospacedDigit()
            }
        } else {
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(metric.averageLabel).font(.caption).foregroundStyle(.secondary)
                    Text(metric.text(st.average, p)).font(.largeTitle.weight(.semibold)).monospacedDigit()
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 4) {
                    ChangeBadge(change: st.change, lowerIsBetter: metric.lowerIsBetter)
                    if st.change != nil {
                        Text("vs previous \(span.label.lowercased())").font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private var dayLabel: String {
        switch metric {
        case .heartRate: return "Average"
        case .battery: return "Lowest"
        default: return "Total"
        }
    }

    private func statCells(_ st: PeriodStats, _ p: Profile, days: Int) -> [(String, String, String?)] {
        var c: [(String, String, String?)] = []
        c.append((metric.averageLabel, metric.text(st.average, p), nil))
        if let t = st.total, metric != .workouts {
            c.append(("Total", metric.text(t, p), nil))
        }
        c.append((metric.lowerIsBetter ? "Lowest" : "Best day", metric.text(st.best, p),
                  st.bestDay.map { Fmt.shortDay($0, cal: app.cal) }))
        c.append(("Days with data", "\(st.daysWithData)", "of \(days)"))
        if metric.goal(p) != nil {
            c.append(("Goal met", "\(st.goalDays) day\(st.goalDays == 1 ? "" : "s")", nil))
        }
        c.append(("Previous \(span.label.lowercased())", metric.text(st.previousAverage, p), "average"))
        return c
    }

    @ViewBuilder
    private func dayExtras(_ d: DaySummary, _ p: Profile) -> some View {
        switch metric {
        case .heartRate, .zoneMinutes:
            Card {
                StatGrid(cells: [
                    ("Average", d.hrAvg.map { "\($0) bpm" } ?? "--", nil),
                    ("Lowest", d.hrMin.map { "\($0) bpm" } ?? "--", nil),
                    ("Highest", d.hrMax.map { "\($0) bpm" } ?? "--", nil),
                    ("Resting", d.restingHR.map { "\($0) bpm" } ?? "--", d.restingEstimated ? "estimated" : nil),
                    ("Zone minutes", "\(d.zoneTotal)", nil),
                    ("Readings", "\(d.heart.count)", "5-minute slots"),
                ])
            }
        case .steps, .distance, .activeMinutes, .calories:
            Card {
                StatGrid(cells: [
                    ("Steps", Fmt.number(d.steps), nil),
                    ("Distance", Metric.distance.text(Metric.distance.value(d, profile: p), p), nil),
                    ("Active minutes", "\(d.activeMinutes)", nil),
                    ("Calories", "\(Fmt.number(d.caloriesTotal))", "\(Fmt.number(d.caloriesActive)) active"),
                    ("Moving", Metric.hm(d.movingMinutes), nil),
                    ("Most active hour", mostActiveHour(d), nil),
                ])
            }
        case .battery:
            Card {
                StatGrid(cells: [
                    ("Lowest", d.battery.map(\.pct).min().map { "\($0)%" } ?? "--", nil),
                    ("Highest", d.battery.map(\.pct).max().map { "\($0)%" } ?? "--", nil),
                    ("Charging", chargingTime(d), nil),
                ])
            }
        default:
            EmptyView()
        }
    }

    private func mostActiveHour(_ d: DaySummary) -> String {
        guard let top = d.stepsHourly.max(), top > 0, let h = d.stepsHourly.firstIndex(of: top) else { return "--" }
        return Fmt.clock(minuteOfDay: h * 60)
    }

    private func chargingTime(_ d: DaySummary) -> String {
        let n = d.battery.filter(\.charging).count
        return n == 0 ? "No" : Metric.hm(n * 5)
    }

    private func zoneCard(_ m: HealthModel, range: ClosedRange<Int>, _ p: Profile) -> some View {
        var total: [HRZone: Int] = [:]
        for day in range where day <= m.today {
            for (z, v) in m.summary(day).zoneMinutes { total[z, default: 0] += v }
        }
        return Card {
            CardTitle(title: "Heart zones", icon: Metric.zoneMinutes.icon, color: Metric.zoneMinutes.color,
                      trailing: "Max \(p.maxHR) bpm")
            ZoneBreakdown(minutes: total, maxHR: p.maxHR)
        }
    }

    private func about(_ p: Profile) -> String? {
        switch metric {
        case .zoneMinutes, .heartRate:
            return "Heart zones are set from your maximum heart rate (\(p.maxHR) bpm): fat burn from 50%, cardio from 70%, peak from 85%. Change it in You."
        case .calories:
            return "Calories add your resting burn, worked out from your age, sex, height and weight, to the energy from your activity."
        case .distance:
            return "Distance is your steps times a stride length worked out from your height."
        case .activeMinutes:
            return "A 5-minute stretch counts as active when you took 500 or more steps in it, or your heart rate averaged 64% of your maximum or more. Workout time always counts."
        case .restingHR:
            return "Resting heart rate comes from the watch's own daily figure. When there isn't one, it's estimated from your lowest heart rate while still, and marked as estimated."
        case .battery:
            return "Week, month and year charts show each day's lowest battery level."
        case .workouts:
            return "Minutes of active workout time recorded on the watch each day."
        default:
            return nil
        }
    }
}
