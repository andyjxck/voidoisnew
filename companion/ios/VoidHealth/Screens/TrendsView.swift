import SwiftUI
import VoidHealthCore

/// every metric for the chosen period, with its average, the change from the
/// period before and a small chart; tap one for its full detail
struct TrendsView: View {
    @Environment(AppModel.self) private var app
    @State private var span: Span = .week

    private let groups: [(String, [Metric])] = [
        ("Activity", [.steps, .distance, .activeMinutes, .calories, .workouts]),
        ("Heart", [.heartRate, .restingHR, .zoneMinutes]),
        ("Sleep", [.sleep, .sleepScore]),
        ("Other", [.water, .battery]),
    ]

    var body: some View {
        let m = app.model
        Screen {
            Picker("Period", selection: $span) {
                Text("Week").tag(Span.week)
                Text("Month").tag(Span.month)
                Text("Year").tag(Span.year)
            }
            .pickerStyle(.segmented)

            Text(Fmt.periodTitle(span, range: m.period(span, page: 0, cal: app.cal), today: m.today, cal: app.cal))
                .font(.subheadline).foregroundStyle(.secondary)

            if m.firstDay == nil {
                Card { EmptyNote(text: "Trends appear once your watch has synced a few days.") }
            }

            ForEach(groups, id: \.0) { g in
                Text(g.0).font(.title3.weight(.semibold)).padding(.top, 6)
                ForEach(g.1) { metric in
                    NavigationLink(value: metric) {
                        TrendRow(metric: metric, span: span)
                    }
                    .buttonStyle(.plain)
                }
            }

            let bests = m.personalBests(cal: app.cal)
            if !bests.isEmpty {
                Text("Personal bests").font(.title3.weight(.semibold)).padding(.top, 6)
                Card {
                    ForEach(bests) { b in
                        BestRow(best: b, cal: app.cal)
                    }
                }
            }
        }
        .navigationTitle("Trends")
        .navigationDestination(for: Metric.self) { metric in
            MetricDetailView(metric: metric, span: span)
        }
    }
}

struct TrendRow: View {
    @Environment(AppModel.self) private var app
    let metric: Metric
    let span: Span

    var body: some View {
        let m = app.model
        let p = m.profile
        let st = m.stats(metric, span: span, page: 0, cal: app.cal)
        let buckets = m.series(metric, span: span, page: 0, cal: app.cal)
        HStack(spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Label(metric.title, systemImage: metric.icon)
                    .font(.subheadline)
                    .foregroundStyle(metric.color)
                Text(metric.text(st.average, p))
                    .font(.title3.weight(.semibold))
                    .monospacedDigit()
                HStack(spacing: 6) {
                    Text(metric.averageLabel).font(.caption).foregroundStyle(.secondary)
                    ChangeBadge(change: st.change, lowerIsBetter: metric.lowerIsBetter)
                }
            }
            Spacer(minLength: 8)
            Sparkline(buckets: buckets, color: metric.color, bars: metric.usesBars)
                .frame(width: 110, height: 48)
            Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
        }
        .padding(14)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

struct BestRow: View {
    let best: PersonalBest
    let cal: DayCalendar

    var body: some View {
        HStack {
            Image(systemName: "trophy.fill").foregroundStyle(.yellow)
            VStack(alignment: .leading, spacing: 1) {
                Text(best.title).font(.subheadline)
                if let d = best.day {
                    Text(Fmt.shortDay(d, cal: cal)).font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            Text(best.value).font(.subheadline.weight(.semibold)).monospacedDigit()
        }
    }
}
