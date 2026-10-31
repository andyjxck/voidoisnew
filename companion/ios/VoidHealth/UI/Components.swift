import SwiftUI
import VoidHealthCore

/// rounded panel used for every block on a screen
struct Card<Content: View>: View {
    var padding: CGFloat = 16
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) { content }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(padding)
            .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }
}

struct CardTitle: View {
    let title: String
    var icon: String?
    var color: Color = .secondary
    var trailing: String?

    var body: some View {
        HStack(spacing: 6) {
            if let icon {
                Image(systemName: icon).foregroundStyle(color)
            }
            Text(title).font(.headline)
            Spacer()
            if let trailing {
                Text(trailing).font(.subheadline).foregroundStyle(.secondary)
            }
        }
    }
}

struct ProgressRing: View {
    let progress: Double
    let color: Color
    var lineWidth: CGFloat = 10

    var body: some View {
        ZStack {
            Circle().stroke(color.opacity(0.18), lineWidth: lineWidth)
            Circle()
                .trim(from: 0, to: min(max(progress, 0), 1))
                .stroke(color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                .rotationEffect(.degrees(-90))
            if progress > 1 {
                // a second lap once past the goal
                Circle()
                    .trim(from: 0, to: min(progress - 1, 1))
                    .stroke(color.opacity(0.6), style: StrokeStyle(lineWidth: lineWidth * 0.5, lineCap: .round))
                    .rotationEffect(.degrees(-90))
            }
        }
        .animation(.easeOut(duration: 0.5), value: progress)
    }
}

struct ProgressBar: View {
    let progress: Double
    let color: Color

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(color.opacity(0.18))
                Capsule().fill(color).frame(width: g.size.width * min(max(progress, 0), 1))
            }
        }
        .frame(height: 6)
    }
}

/// one metric on the Today grid
struct StatTile: View {
    let metric: Metric
    let value: String
    var detail: String?
    var progress: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: metric.icon).foregroundStyle(metric.color)
                Text(metric.title).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
            }
            Text(value)
                .font(.title3.weight(.semibold))
                .monospacedDigit()
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            if let progress {
                ProgressBar(progress: progress, color: metric.color)
            }
            if let detail {
                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 96, alignment: .topLeading)
        .padding(14)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

/// label over a value, for stat grids
struct StatCell: View {
    let label: String
    let value: String
    var detail: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.body.weight(.semibold)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.7)
            if let detail {
                Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct StatGrid: View {
    let cells: [(String, String, String?)]

    var body: some View {
        LazyVGrid(columns: [GridItem(.flexible(), alignment: .topLeading), GridItem(.flexible(), alignment: .topLeading),
                            GridItem(.flexible(), alignment: .topLeading)],
                  alignment: .leading, spacing: 14) {
            ForEach(cells.indices, id: \.self) { i in
                StatCell(label: cells[i].0, value: cells[i].1, detail: cells[i].2)
            }
        }
    }
}

/// change against the previous period, green when it moved the good way
struct ChangeBadge: View {
    let change: Double?
    var lowerIsBetter = false

    var body: some View {
        if let change, change.isFinite {
            let pct = Int(change.rounded())          // already a percentage
            let good = lowerIsBetter ? pct < 0 : pct > 0
            HStack(spacing: 2) {
                Image(systemName: pct == 0 ? "equal" : pct > 0 ? "arrow.up" : "arrow.down")
                Text(verbatim: "\(abs(pct))%")
            }
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(pct == 0 ? Color.secondary : good ? Color.green : Color.orange)
            .background(Color(.tertiarySystemFill), in: Capsule())
        }
    }
}

/// "< title >" for paging through days and periods
struct PeriodHeader: View {
    let title: String
    var subtitle: String?
    let canBack: Bool
    let canForward: Bool
    let back: () -> Void
    let forward: () -> Void

    var body: some View {
        HStack {
            Button(action: back) { Image(systemName: "chevron.left").frame(width: 36, height: 36) }
                .disabled(!canBack)
            Spacer()
            VStack(spacing: 0) {
                Text(title).font(.headline)
                if let subtitle {
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            Button(action: forward) { Image(systemName: "chevron.right").frame(width: 36, height: 36) }
                .disabled(!canForward)
        }
        .buttonStyle(.borderless)
    }
}

/// heart zone minutes as one stacked bar with a legend
struct ZoneBreakdown: View {
    let minutes: [HRZone: Int]
    let maxHR: Int

    var body: some View {
        let zones: [HRZone] = [.fatBurn, .cardio, .peak]
        let total = max(1, zones.reduce(0) { $0 + (minutes[$1] ?? 0) })
        VStack(alignment: .leading, spacing: 10) {
            GeometryReader { g in
                HStack(spacing: 2) {
                    ForEach(zones, id: \.self) { z in
                        let m = minutes[z] ?? 0
                        if m > 0 {
                            Rectangle().fill(z.color)
                                .frame(width: max(4, g.size.width * Double(m) / Double(total) - 2))
                        }
                    }
                }
            }
            .frame(height: 10)
            .clipShape(Capsule())
            .background(Capsule().fill(Color(.tertiarySystemFill)))
            ForEach(zones, id: \.self) { z in
                let r = z.range(maxHR: maxHR)
                HStack {
                    Circle().fill(z.color).frame(width: 8, height: 8)
                    Text(z.name)
                    Text(z == .peak ? "\(r.lowerBound)+ bpm" : "\(r.lowerBound)–\(r.upperBound) bpm")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(Metric.hm(minutes[z] ?? 0)).monospacedDigit()
                }
                .font(.subheadline)
            }
        }
    }
}

/// connection state, last sync and a sync button
struct SyncBar: View {
    @Environment(AppModel.self) private var app

    var body: some View {
        let link = app.link
        HStack(spacing: 10) {
            Image(systemName: icon(link.status))
                .foregroundStyle(link.status.isProblem ? Color.orange : link.isConnected ? Color.teal : Color.secondary)
                .symbolEffect(.pulse, isActive: link.status == .syncing || link.status == .connecting)
            VStack(alignment: .leading, spacing: 1) {
                Text(link.status == .upToDate ? (link.watchName ?? "Watch") + " · Up to date" : link.status.text)
                    .font(.subheadline.weight(.medium))
                if let last = link.lastSync {
                    Text("Synced \(last.formatted(.relative(presentation: .named)))")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text("Not synced yet").font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            if let b = link.battery {
                Label("\(b)%" as String, systemImage: b <= 20 ? "battery.25percent" : "battery.75percent")
                    .font(.caption).foregroundStyle(b <= 20 ? Color.orange : Color.secondary)
            }
            Button {
                app.link.refresh()
            } label: {
                Image(systemName: "arrow.triangle.2.circlepath")
            }
            .buttonStyle(.borderless)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    private func icon(_ s: WatchLink.Status) -> String {
        switch s {
        case .bluetoothOff, .unauthorized, .unsupported: return "exclamationmark.triangle.fill"
        case .needsFirmware: return "arrow.down.circle"
        case .searching, .waiting: return "applewatch.radiowaves.left.and.right"
        case .connecting, .syncing: return "arrow.triangle.2.circlepath"
        case .upToDate: return "checkmark.circle.fill"
        }
    }
}

struct EmptyNote: View {
    let text: String
    var icon = "chart.bar.xaxis"

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: icon).foregroundStyle(.secondary)
            Text(text).font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 6)
    }
}

/// screen background and spacing shared by every tab
struct Screen<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 14) { content }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
        }
        .background(Color(.systemGroupedBackground))
    }
}
