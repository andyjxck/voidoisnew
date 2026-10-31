import SwiftUI
import UIKit
import VoidHealthCore

/// profile, goals, records, the watch, and the data on this phone
struct YouView: View {
    @Environment(AppModel.self) private var app
    @State private var export: ExportFile?
    @State private var exportFailed = false
    @State private var confirmErase = false
    @State private var confirmForget = false
    @AppStorage(GoalNotifier.enabledKey) private var goalNotifications = true

    var body: some View {
        @Bindable var app = app
        let m = app.model
        let p = app.profile
        Form {
            Section("Profile") {
                Stepper("Age: \(p.age)", value: $app.profile.age, in: 13...100)
                Picker("Sex", selection: $app.profile.sex) {
                    Text("Male").tag(Profile.Sex.male)
                    Text("Female").tag(Profile.Sex.female)
                    Text("Not set").tag(Profile.Sex.other)
                }
                Stepper("Height: \(Fmt.height(p.heightCm, metric: p.useMetric))", value: $app.profile.heightCm,
                        in: 120...230, step: p.useMetric ? 1 : 2.54)
                Stepper("Weight: \(Fmt.weight(p.weightKg, metric: p.useMetric))", value: $app.profile.weightKg,
                        in: 30...250, step: p.useMetric ? 0.5 : 0.453592)
                Toggle("Metric units", isOn: $app.profile.useMetric)
            }

            Section {
                Stepper("Steps: \(Fmt.number(p.stepGoal))", value: $app.profile.stepGoal, in: 1000...50000, step: 500)
                Stepper("Active minutes: \(p.activeMinuteGoal)", value: $app.profile.activeMinuteGoal, in: 5...300, step: 5)
                Stepper("Water: \(Metric.water.format(Double(p.waterGoalML), profile: p))", value: $app.profile.waterGoalML,
                        in: 250...6000, step: 250)
                Stepper("Sleep: \(Metric.hm(p.sleepGoalMinutes))", value: $app.profile.sleepGoalMinutes,
                        in: 240...720, step: 15)
                Stepper(p.calorieGoal > 0 ? "Calories: \(Fmt.number(p.calorieGoal)) kcal" : "Calories: no goal",
                        value: $app.profile.calorieGoal, in: 0...6000, step: 100)
                Toggle("Notify me when I hit a goal", isOn: $goalNotifications)
                    .onChange(of: goalNotifications) { _, on in
                        if on { GoalNotifier.requestIfNeeded() }
                    }
            } header: {
                Text("Daily goals")
            } footer: {
                Text("Closing a ring celebrates once a day, on Today and on your watch. The watch gets your step and water goals when it syncs.")
            }

            Section {
                Toggle("Set my own maximum", isOn: Binding(
                    get: { app.profile.maxHROverride > 0 },
                    set: { on in app.profile.maxHROverride = on ? app.profile.maxHR : 0 }))
                if p.maxHROverride > 0 {
                    Stepper("Max heart rate: \(p.maxHR) bpm", value: $app.profile.maxHROverride, in: 100...230)
                } else {
                    LabeledContent("Max heart rate", value: "\(p.maxHR) bpm, from your age")
                }
                ForEach([HRZone.fatBurn, .cardio, .peak], id: \.self) { z in
                    let r = z.range(maxHR: p.maxHR)
                    LabeledContent {
                        Text(z == .peak ? "\(r.lowerBound)+ bpm" : "\(r.lowerBound)–\(r.upperBound) bpm").monospacedDigit()
                    } label: {
                        Label(z.name, systemImage: "circle.fill").foregroundStyle(z.color)
                    }
                }
            } header: {
                Text("Heart zones")
            } footer: {
                Text("Set your own maximum if you know it from a test. Otherwise it's 220 minus your age.")
            }

            Section("Streaks") {
                ForEach([Metric.steps, .activeMinutes, .water, .sleep], id: \.self) { metric in
                    let s = m.streak(metric)
                    LabeledContent {
                        Text("\(s.current) now · best \(s.longest)").monospacedDigit()
                    } label: {
                        Label(metric.title, systemImage: metric.icon)
                    }
                }
            }

            let bests = m.personalBests(cal: app.cal)
            Section("Personal bests") {
                if bests.isEmpty {
                    Text("Your best days show up here once you have some history.").foregroundStyle(.secondary)
                }
                ForEach(bests) { b in
                    BestRow(best: b, cal: app.cal)
                }
            }

            watchSection

            Section {
                Button {
                    share(app.exportDailyCSV())
                } label: {
                    Label("Export daily summary (CSV)", systemImage: "square.and.arrow.up")
                }
                Button {
                    share(app.exportRecordsCSV())
                } label: {
                    Label("Export every record (CSV)", systemImage: "tablecells")
                }
                LabeledContent("Records on this iPhone", value: Fmt.number(app.recordCount))
                if let f = m.firstDay {
                    LabeledContent("History since", value: app.cal.date(f).formatted(date: .abbreviated, time: .omitted))
                }
                Button("Erase history on this iPhone", role: .destructive) { confirmErase = true }
            } header: {
                Text("Your data")
            } footer: {
                Text("Everything stays on this iPhone. Nothing is uploaded anywhere.")
            }

            Section("About") {
                LabeledContent("Version", value: Self.version)
                LabeledContent("Works with", value: "PineTime with VoidOS 2.3+")
            }
        }
        .navigationTitle("You")
        .sheet(item: $export) { f in
            ShareSheet(url: f.url).ignoresSafeArea()
        }
        .alert("Nothing to export yet", isPresented: $exportFailed) {
            Button("OK", role: .cancel) {}
        }
        .confirmationDialog("Erase the history on this iPhone?", isPresented: $confirmErase, titleVisibility: .visible) {
            Button("Erase", role: .destructive) { app.eraseHistory() }
        } message: {
            Text("Records your watch has already handed over and deleted can't be brought back. Export them first if you want a copy.")
        }
        .confirmationDialog("Forget this watch?", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Forget watch", role: .destructive) { app.link.forgetWatch() }
        } message: {
            Text("The app disconnects and looks for a watch again. Your history stays.")
        }
    }

    private var watchSection: some View {
        let link = app.link
        return Section {
            LabeledContent("Watch", value: link.watchName ?? "None yet")
            LabeledContent("Status", value: link.status.text)
            if let fw = link.firmware {
                LabeledContent("Firmware", value: fw)
            }
            if let b = link.battery {
                LabeledContent("Battery", value: "\(b)%")
            }
            if let bpm = link.liveBPM {
                LabeledContent("Heart rate now", value: "\(bpm) bpm")
            }
            LabeledContent("Last sync", value: link.lastSync.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "Never")
            if link.receivedThisSession > 0 {
                LabeledContent("Received this connection", value: Fmt.number(link.receivedThisSession))
            }
            if let i = link.info {
                LabeledContent("Waiting on the watch", value: Fmt.number(i.pending))
                LabeledContent("Watch storage used", value: "\(Int((i.fill * 100).rounded()))%")
                LabeledContent("Watch holds up to", value: "\(Fmt.number(Int(i.capacity))) records")
                if i.dropped > 0 {
                    LabeledContent("Lost when the watch was full", value: Fmt.number(Int(i.dropped)))
                        .foregroundStyle(.orange)
                }
            }
            if let e = link.lastError {
                Text(e).font(.footnote).foregroundStyle(.orange)
            }
            Button("Sync now") { app.link.refresh() }
            if link.hasWatch {
                Button("Forget watch", role: .destructive) { confirmForget = true }
            }
        } header: {
            Text("Watch")
        } footer: {
            Text("The watch keeps everything until this iPhone confirms it has saved it, so nothing is lost if you're out of range for a while.")
        }
    }

    private func share(_ url: URL?) {
        if let url {
            export = ExportFile(url: url)
        } else {
            exportFailed = true
        }
    }

    private static var version: String {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0"
        let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "1"
        return "\(v) (\(b))"
    }
}

struct ExportFile: Identifiable {
    let id = UUID()
    let url: URL
}

struct ShareSheet: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: [url], applicationActivities: nil)
    }

    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}
