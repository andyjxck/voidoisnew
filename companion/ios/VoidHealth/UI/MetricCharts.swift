import Charts
import SwiftUI
import VoidHealthCore

extension Span {
    var label: String {
        switch self {
        case .day: return "Day"
        case .week: return "Week"
        case .month: return "Month"
        case .year: return "Year"
        }
    }

    /// bar width / selection unit for this span's buckets
    var bucketUnit: Calendar.Component {
        switch self {
        case .day: return .hour
        case .week, .month: return .day
        case .year: return .month
        }
    }
}

extension Metric {
    /// spans that make sense: a once-a-day value has no day chart
    var spans: [Span] {
        switch self {
        case .restingHR, .sleep, .sleepScore, .workouts: return [.week, .month, .year]
        default: return Span.allCases
        }
    }

    /// short axis label
    func axis(_ v: Double, _ p: Profile) -> String {
        switch self {
        case .sleep: return "\(Int((v / 60).rounded()))h"
        case .distance: return String(format: v < 10 ? "%.1f" : "%.0f", v)
        case .water: return v >= 1000 ? String(format: "%.1fL", v / 1000) : "\(Int(v))"
        default:
            if v >= 10_000 { return "\(Int((v / 1000).rounded()))k" }
            if v >= 1000 { return String(format: "%.1fk", v / 1000) }
            return "\(Int(v.rounded()))"
        }
    }
}

/// the main chart on a metric's detail screen; drag across it to read values
struct MetricChart: View {
    let metric: Metric
    let span: Span
    let buckets: [Bucket]
    let domain: ClosedRange<Date>
    let profile: Profile
    var goal: Double?
    var height: CGFloat = 220

    @State private var selected: Date?

    private var points: [Bucket] { buckets.filter { $0.value != nil } }
    private var bars: Bool { span == .day ? (metric.isTotal && metric != .workouts) : metric.usesBars }
    private var rangeHR: Bool { metric == .heartRate || (span == .day && (metric == .restingHR || metric == .zoneMinutes)) }

    private var picked: Bucket? {
        guard let selected, !points.isEmpty else { return nil }
        return points.min { abs($0.date.timeIntervalSince(selected)) < abs($1.date.timeIntervalSince(selected)) }
    }

    private var strideUnit: Calendar.Component {
        switch span {
        case .day: return .hour
        case .week, .month: return .day
        case .year: return .month
        }
    }

    private var strideCount: Int {
        switch span {
        case .day: return 6
        case .week: return 1
        case .month: return 7
        case .year: return 2
        }
    }

    private var axisFormat: Date.FormatStyle {
        switch span {
        case .day: return .dateTime.hour()
        case .week: return .dateTime.weekday(.narrow)
        case .month: return .dateTime.day()
        case .year: return .dateTime.month(.abbreviated)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            readout
            if points.isEmpty {
                ZStack {
                    RoundedRectangle(cornerRadius: 12).fill(Color(.tertiarySystemFill).opacity(0.4))
                    Text("No data").font(.subheadline).foregroundStyle(.secondary)
                }
                .frame(height: height)
            } else {
                chart.frame(height: height)
            }
        }
    }

    /// value under the finger, or a hint
    private var readout: some View {
        HStack {
            if let b = picked, let v = b.value {
                Text(readoutDate(b.date)).foregroundStyle(.secondary)
                Spacer()
                if rangeHR, let lo = b.low, let hi = b.high, span != .day {
                    Text("\(Int(lo))–\(Int(hi)) bpm · avg \(Int(v))").fontWeight(.semibold)
                } else if span == .day && rangeHR, let hi = b.high {
                    Text("\(Int(v)) bpm · max \(Int(hi))").fontWeight(.semibold)
                } else {
                    Text(metric.text(v, profile)).fontWeight(.semibold)
                }
            } else {
                Text("Touch and drag the chart for values").foregroundStyle(.tertiary)
                Spacer()
            }
        }
        .font(.caption)
        .monospacedDigit()
        .frame(height: 16)
    }

    private func readoutDate(_ d: Date) -> String {
        switch span {
        case .day:
            if metric.isTotal {
                let end = d.addingTimeInterval(3600)
                return "\(Fmt.time(d)) – \(Fmt.time(end))"
            }
            return Fmt.time(d)
        case .week, .month: return d.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated))
        case .year: return d.formatted(.dateTime.month(.wide).year()) + (metric.isTotal ? " · daily avg" : "")
        }
    }

    private var chart: some View {
        Chart {
            if bars {
                ForEach(points) { b in
                    BarMark(x: .value("Date", b.date, unit: span.bucketUnit),
                            y: .value(metric.title, b.value ?? 0))
                        .foregroundStyle(barColor(b))
                        .cornerRadius(3)
                }
            } else if rangeHR {
                ForEach(points) { b in
                    if span == .day {
                        AreaMark(x: .value("Time", b.date),
                                 yStart: .value("Average", b.low ?? b.value ?? 0),
                                 yEnd: .value("Max", b.high ?? b.value ?? 0))
                            .foregroundStyle(metric.color.opacity(0.25))
                            .interpolationMethod(.monotone)
                    } else {
                        RuleMark(x: .value("Date", b.date, unit: span.bucketUnit),
                                 yStart: .value("Low", b.low ?? b.value ?? 0),
                                 yEnd: .value("High", b.high ?? b.value ?? 0))
                            .lineStyle(StrokeStyle(lineWidth: span == .week ? 8 : 4, lineCap: .round))
                            .foregroundStyle(metric.color.opacity(0.45))
                    }
                }
                if span == .day {
                    ForEach(points) { b in
                        LineMark(x: .value("Time", b.date), y: .value("Average", b.value ?? 0))
                            .foregroundStyle(metric.color)
                            .lineStyle(StrokeStyle(lineWidth: 1.5))
                            .interpolationMethod(.monotone)
                    }
                } else {
                    ForEach(points) { b in
                        PointMark(x: .value("Date", b.date, unit: span.bucketUnit), y: .value("Average", b.value ?? 0))
                            .foregroundStyle(metric.color)
                            .symbolSize(span == .week ? 40 : 16)
                    }
                }
            } else {
                ForEach(points) { b in
                    LineMark(x: .value("Date", b.date), y: .value(metric.title, b.value ?? 0))
                        .foregroundStyle(metric.color)
                        .interpolationMethod(.monotone)
                    PointMark(x: .value("Date", b.date), y: .value(metric.title, b.value ?? 0))
                        .foregroundStyle(metric.color)
                        .symbolSize(span == .day ? 0 : 24)
                }
            }
            if let goal, span != .day || metric == .water {
                RuleMark(y: .value("Goal", goal))
                    .foregroundStyle(Color.secondary)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 4]))
            }
            if let b = picked {
                RuleMark(x: .value("Selected", b.date, unit: span == .day && !bars ? .minute : span.bucketUnit))
                    .foregroundStyle(Color.primary.opacity(0.25))
            }
        }
        .chartXScale(domain: domain)
        .chartYScale(domain: .automatic(includesZero: bars))
        .chartXSelection(value: $selected)
        .chartXAxis {
            AxisMarks(values: .stride(by: strideUnit, count: strideCount)) { _ in
                AxisGridLine()
                AxisValueLabel(format: axisFormat)
            }
        }
        .chartYAxis {
            AxisMarks(position: .trailing) { v in
                AxisGridLine()
                AxisValueLabel {
                    if let d = v.as(Double.self) {
                        Text(metric.axis(d, profile))
                    }
                }
            }
        }
    }

    private func barColor(_ b: Bucket) -> Color {
        if let goal, span != .day, let v = b.value, v >= goal { return metric.color }
        return metric.color.opacity(span == .day ? 1 : 0.6)
    }
}

/// 24 hourly bars for the Today cards
struct HourlyBars: View {
    let values: [Int]
    let day: Date
    let color: Color
    var height: CGFloat = 110

    var body: some View {
        Chart {
            ForEach(values.indices, id: \.self) { h in
                BarMark(x: .value("Hour", day.addingTimeInterval(Double(h) * 3600), unit: .hour),
                        y: .value("Value", values[h]))
                    .foregroundStyle(color)
                    .cornerRadius(2)
            }
        }
        .chartXScale(domain: day...day.addingTimeInterval(86400))
        .chartXAxis {
            AxisMarks(values: .stride(by: .hour, count: 6)) { _ in
                AxisValueLabel(format: .dateTime.hour())
            }
        }
        .chartYAxis(.hidden)
        .frame(height: height)
    }
}

/// a day's heart rate: shaded average-to-max band with the average line
struct HeartDayChart: View {
    let points: [HeartPoint]
    let day: Date
    var restingHR: Int?
    var height: CGFloat = 150

    var body: some View {
        Chart {
            ForEach(points, id: \.second) { p in
                AreaMark(x: .value("Time", day.addingTimeInterval(Double(p.second))),
                         yStart: .value("Average", p.avg),
                         yEnd: .value("Max", p.max))
                    .foregroundStyle(Color.red.opacity(0.22))
                    .interpolationMethod(.monotone)
            }
            ForEach(points, id: \.second) { p in
                LineMark(x: .value("Time", day.addingTimeInterval(Double(p.second))), y: .value("Average", p.avg))
                    .foregroundStyle(Color.red)
                    .lineStyle(StrokeStyle(lineWidth: 1.5))
                    .interpolationMethod(.monotone)
            }
            if let r = restingHR {
                RuleMark(y: .value("Resting", r))
                    .foregroundStyle(Color.pink.opacity(0.7))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
            }
        }
        .chartXScale(domain: day...day.addingTimeInterval(86400))
        .chartYScale(domain: .automatic(includesZero: false))
        .chartXAxis {
            AxisMarks(values: .stride(by: .hour, count: 6)) { _ in
                AxisGridLine()
                AxisValueLabel(format: .dateTime.hour())
            }
        }
        .chartYAxis {
            AxisMarks(position: .trailing, values: .automatic(desiredCount: 3))
        }
        .frame(height: height)
    }
}

/// heart rate over a window of a day (a workout)
struct HeartWindowChart: View {
    let points: [HeartPoint]
    let day: Date
    let from: Date
    let to: Date
    var marks: ClosedRange<Date>?
    var height: CGFloat = 170

    var body: some View {
        Chart {
            if let marks, let lo = points.map(\.avg).min(), let hi = points.map(\.max).max() {
                RectangleMark(xStart: .value("Start", marks.lowerBound), xEnd: .value("End", marks.upperBound),
                              yStart: .value("Low", lo), yEnd: .value("High", hi))
                    .foregroundStyle(Color.mint.opacity(0.12))
            }
            ForEach(points, id: \.second) { p in
                LineMark(x: .value("Time", day.addingTimeInterval(Double(p.second))), y: .value("Average", p.avg))
                    .foregroundStyle(by: .value("Series", "Average"))
                    .interpolationMethod(.monotone)
                LineMark(x: .value("Time", day.addingTimeInterval(Double(p.second))), y: .value("Max", p.max))
                    .foregroundStyle(by: .value("Series", "Max"))
                    .interpolationMethod(.monotone)
            }
        }
        .chartForegroundStyleScale(["Average": Color.red, "Max": Color.orange.opacity(0.7)])
        .chartXScale(domain: from...to)
        .chartYScale(domain: .automatic(includesZero: false))
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: 4)) { _ in
                AxisGridLine()
                AxisValueLabel(format: .dateTime.hour().minute())
            }
        }
        .chartYAxis {
            AxisMarks(position: .trailing, values: .automatic(desiredCount: 4))
        }
        .chartLegend(position: .top, alignment: .leading)
        .frame(height: height)
    }
}

/// one night, 5-minute slots: how much the wearer moved, and heart rate
struct SleepNightChart: View {
    let session: SleepSession
    let bed: Date

    private var end: Date { bed.addingTimeInterval(Double(session.seconds)) }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Movement").font(.caption).foregroundStyle(.secondary)
                Chart {
                    ForEach(session.movement.indices, id: \.self) { i in
                        BarMark(x: .value("Time", bed.addingTimeInterval(Double(i) * 300), unit: .minute),
                                y: .value("Moving", max(session.movement[i], 1)))
                            .foregroundStyle(slotColor(session.movement[i]))
                    }
                }
                .chartXScale(domain: bed...max(end, bed.addingTimeInterval(600)))
                .chartXAxis {
                    AxisMarks(values: .automatic(desiredCount: 5)) { _ in
                        AxisValueLabel(format: .dateTime.hour().minute())
                    }
                }
                .chartYAxis(.hidden)
                .frame(height: 90)
                HStack(spacing: 14) {
                    legend(Color.indigo.opacity(0.35), "Still")
                    legend(Color.indigo.opacity(0.9), "Light movement")
                    legend(Color.orange, "Restless")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }
            let hr: [(Int, Int)] = session.heart.enumerated().filter { $0.element > 0 }.map { ($0.offset, $0.element) }
            if !hr.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Heart rate").font(.caption).foregroundStyle(.secondary)
                    Chart {
                        ForEach(hr.indices, id: \.self) { j in
                            LineMark(x: .value("Time", bed.addingTimeInterval(Double(hr[j].0) * 300)),
                                     y: .value("BPM", hr[j].1))
                                .foregroundStyle(Color.red)
                                .interpolationMethod(.monotone)
                        }
                    }
                    .chartXScale(domain: bed...max(end, bed.addingTimeInterval(600)))
                    .chartYScale(domain: .automatic(includesZero: false))
                    .chartXAxis {
                        AxisMarks(values: .automatic(desiredCount: 5)) { _ in
                            AxisGridLine()
                            AxisValueLabel(format: .dateTime.hour().minute())
                        }
                    }
                    .chartYAxis {
                        AxisMarks(position: .trailing, values: .automatic(desiredCount: 3))
                    }
                    .frame(height: 110)
                }
            }
        }
    }
}

extension SleepNightChart {
    /// the same 30-second threshold the model uses for restless minutes
    fileprivate func slotColor(_ moving: Int) -> Color {
        if moving >= 30 { return .orange }
        return moving > 0 ? Color.indigo.opacity(0.9) : Color.indigo.opacity(0.35)
    }

    fileprivate func legend(_ c: Color, _ t: String) -> some View {
        HStack(spacing: 4) {
            RoundedRectangle(cornerRadius: 2).fill(c).frame(width: 10, height: 10)
            Text(t)
        }
    }
}

/// small bars for list rows
struct Sparkline: View {
    let buckets: [Bucket]
    let color: Color
    let bars: Bool

    var body: some View {
        let pts = buckets.filter { $0.value != nil }
        Chart {
            ForEach(pts) { b in
                if bars {
                    BarMark(x: .value("i", b.index), y: .value("v", b.value ?? 0))
                        .foregroundStyle(color)
                        .cornerRadius(1.5)
                } else {
                    LineMark(x: .value("i", b.index), y: .value("v", b.value ?? 0))
                        .foregroundStyle(color)
                        .interpolationMethod(.monotone)
                }
            }
        }
        .chartXScale(domain: 0...max(1, buckets.count - 1))
        .chartYScale(domain: .automatic(includesZero: bars))
        .chartXAxis(.hidden)
        .chartYAxis(.hidden)
    }
}
