import SwiftUI
import VoidHealthCore

/// the week's training and every workout the watch has logged
struct WorkoutsView: View {
    @Environment(AppModel.self) private var app
    @State private var shown = 30

    var body: some View {
        let m = app.model
        let p = m.profile
        let all = allWorkouts(m)
        let range = m.period(.week, page: 0, cal: app.cal)
        let week = all.filter { range.contains($0.day) }
        Screen {
            Card {
                NavigationLink(value: Metric.workouts) {
                    HStack {
                        CardTitle(title: "This week", icon: Metric.workouts.icon, color: Metric.workouts.color)
                        Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
                    }
                }
                .buttonStyle(.plain)
                StatGrid(cells: [
                    ("Workouts", "\(week.count)", nil),
                    ("Active time", Metric.hm(week.reduce(0) { $0 + $1.activeSeconds } / 60), nil),
                    ("Calories", Fmt.number(week.reduce(0) { $0 + $1.calories }), "kcal"),
                    ("Sets", "\(week.reduce(0) { $0 + $1.sets })", nil),
                    ("Avg heart rate", avgHR(week).map { "\($0) bpm" } ?? "--", nil),
                    ("Peak heart rate", week.map(\.maxBPM).max().map { $0 > 0 ? "\($0) bpm" : "--" } ?? "--", nil),
                ])
                MetricChart(metric: .workouts, span: .week, buckets: m.series(.workouts, span: .week, page: 0, cal: app.cal),
                            domain: app.cal.date(range.lowerBound)...app.cal.date(range.upperBound + 1),
                            profile: p, height: 130)
            }

            let mix = routineMix(all.filter { $0.day > m.today - 30 })
            if !mix.isEmpty {
                Card {
                    CardTitle(title: "Last 30 days", icon: "chart.pie", color: .mint)
                    ForEach(mix, id: \.0) { r in
                        HStack {
                            Text(r.0)
                            Spacer()
                            Text("\(r.1)× · \(Metric.hm(r.2))").foregroundStyle(.secondary).monospacedDigit()
                        }
                        .font(.subheadline)
                    }
                }
            }

            if all.isEmpty {
                Card {
                    EmptyNote(text: "No workouts yet. Start one from the workout app on your watch and it shows up here after the next sync.",
                              icon: "dumbbell")
                }
            } else {
                Text("History").font(.title3.weight(.semibold)).padding(.top, 6)
                Card {
                    ForEach(all.prefix(shown)) { w in
                        NavigationLink(value: w) {
                            WorkoutRow(w: w, cal: app.cal, showDate: true)
                        }
                        .buttonStyle(.plain)
                        if w.id != all.prefix(shown).last?.id { Divider() }
                    }
                    if all.count > shown {
                        Button("Show more") { shown += 30 }
                            .frame(maxWidth: .infinity)
                    }
                }
            }
        }
        .navigationTitle("Workouts")
        .navigationDestination(for: Workout.self) { w in
            WorkoutDetailView(workout: w)
        }
        .navigationDestination(for: Metric.self) { metric in
            MetricDetailView(metric: metric)
        }
    }

    private func allWorkouts(_ m: HealthModel) -> [Workout] {
        m.days.values.flatMap(\.workouts).sorted { $0.start > $1.start }
    }

    private func avgHR(_ ws: [Workout]) -> Int? {
        let v = ws.map(\.avgBPM).filter { $0 > 0 }
        return v.isEmpty ? nil : v.reduce(0, +) / v.count
    }

    private func routineMix(_ ws: [Workout]) -> [(String, Int, Int)] {
        var d: [String: (Int, Int)] = [:]
        for w in ws {
            let c = d[w.routineName] ?? (0, 0)
            d[w.routineName] = (c.0 + 1, c.1 + w.activeSeconds / 60)
        }
        return d.map { ($0.key, $0.value.0, $0.value.1) }.sorted { $0.1 > $1.1 }
    }
}

struct WorkoutRow: View {
    let w: Workout
    let cal: DayCalendar
    var showDate = false

    var body: some View {
        let start = cal.date(watchTime: w.start)
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(Metric.workouts.color)
                .frame(width: 40, height: 40)
                .background(Metric.workouts.color.opacity(0.15), in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(w.routineName).font(.subheadline.weight(.semibold))
                Text(showDate ? start.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated).hour().minute())
                              : Fmt.time(start))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(Metric.hm(w.activeSeconds / 60)).font(.subheadline.weight(.medium)).monospacedDigit()
                Text(detail).font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
            Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
        }
        .contentShape(Rectangle())
    }

    private var icon: String {
        switch w.routine {
        case 0: return "figure.strengthtraining.traditional"
        case 1: return "figure.rower"
        case 2: return "figure.step.training"
        default: return "figure.mixed.cardio"
        }
    }

    private var detail: String {
        var parts: [String] = []
        if w.sets > 0 { parts.append("\(w.sets) sets") }
        if w.avgBPM > 0 { parts.append("\(w.avgBPM) bpm") }
        if w.calories > 0 { parts.append("\(w.calories) kcal") }
        return parts.joined(separator: " · ")
    }
}

struct WorkoutDetailView: View {
    @Environment(AppModel.self) private var app
    let workout: Workout

    var body: some View {
        let w = workout
        let p = app.model.profile
        let d = app.model.summary(w.day)
        let dayStart = app.cal.date(w.day)
        let start = app.cal.date(watchTime: w.start)
        let activeEnd = start.addingTimeInterval(Double(max(w.activeSeconds, 60)))
        let from = start.addingTimeInterval(-10 * 60)
        let to = activeEnd.addingTimeInterval(20 * 60)
        let fromSec = Int(from.timeIntervalSince(dayStart)), toSec = Int(to.timeIntervalSince(dayStart))
        let pts = d.heart.filter { $0.second >= fromSec && $0.second <= toSec }
        let zone = HRZone.of(bpm: w.avgBPM, maxHR: p.maxHR)
        Screen {
            Card {
                HStack(spacing: 14) {
                    WorkoutRow(w: w, cal: app.cal, showDate: true)
                }
                StatGrid(cells: [
                    ("Active time", Metric.hm(w.activeSeconds / 60), nil),
                    ("Sets", w.sets > 0 ? "\(w.sets)" : "--", nil),
                    ("Calories", w.calories > 0 ? "\(w.calories) kcal" : "--", nil),
                    ("Average HR", w.avgBPM > 0 ? "\(w.avgBPM) bpm" : "--", w.avgBPM > 0 ? zone.name : nil),
                    ("Max HR", w.maxBPM > 0 ? "\(w.maxBPM) bpm" : "--",
                     w.maxBPM > 0 ? "\(w.maxBPM * 100 / max(1, p.maxHR))% of max" : nil),
                    ("Started", Fmt.time(start), nil),
                ])
            }
            Card {
                CardTitle(title: "Heart rate", icon: "heart.fill", color: .red, trailing: "5-minute readings")
                if pts.count >= 2 {
                    HeartWindowChart(points: pts, day: dayStart, from: from, to: to, marks: start...activeEnd)
                    Text("Shaded: the workout's active time. The watch logs heart rate every 5 minutes, so short efforts are averaged.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    EmptyNote(text: "Not enough heart readings around this workout.", icon: "heart")
                }
            }
            Card {
                CardTitle(title: "Heart zones", icon: Metric.zoneMinutes.icon, color: Metric.zoneMinutes.color)
                ForEach([HRZone.fatBurn, .cardio, .peak], id: \.self) { z in
                    let r = z.range(maxHR: p.maxHR)
                    HStack {
                        Circle().fill(z.color).frame(width: 8, height: 8)
                        Text(z.name)
                        Spacer()
                        Text(z == .peak ? "\(r.lowerBound)+ bpm" : "\(r.lowerBound)–\(r.upperBound) bpm")
                            .foregroundStyle(.secondary).monospacedDigit()
                        if z == zone && w.avgBPM > 0 {
                            Image(systemName: "checkmark").foregroundStyle(z.color)
                        }
                    }
                    .font(.subheadline)
                }
            }
        }
        .navigationTitle(w.routineName)
        .navigationBarTitleDisplayMode(.inline)
    }
}
