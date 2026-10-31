import Foundation
import Observation
import VoidHealthCore

/// App state: the stored records, the wearer's profile and goals, the
/// derived model the screens read, and the link to the watch.
@MainActor
@Observable
final class AppModel {
    var profile: Profile {
        didSet {
            if let data = try? JSONEncoder().encode(profile) {
                UserDefaults.standard.set(data, forKey: "profile")
            }
            rebuild()
        }
    }
    private(set) var model: HealthModel
    private(set) var storeError: String?
    let cal = DayCalendar()
    let store: RecordStore
    let link: WatchLink

    @ObservationIgnored private var rebuildTask: Task<Void, Never>?
    @ObservationIgnored private var generation = 0

    init() {
        var p = Profile()
        if let data = UserDefaults.standard.data(forKey: "profile"),
           let saved = try? JSONDecoder().decode(Profile.self, from: data) {
            p = saved
        }
        profile = p
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VoidHealth", isDirectory: true)
        let s: RecordStore
        var err: String?
        do {
            s = try RecordStore(url: dir.appendingPathComponent("records.bin"))
        } catch {
            err = "Could not open the history file: \(error.localizedDescription)"
            s = try! RecordStore(url: FileManager.default.temporaryDirectory
                .appendingPathComponent("VoidHealth-records.bin"))
        }
        store = s
        storeError = err
        model = HealthModel(profile: p, today: DayCalendar().day(of: Date()))
        link = WatchLink(store: s)
        link.onNewRecords = { [weak self] in self?.scheduleRebuild() }
        link.onLiveSteps = { [weak self] in self?.scheduleRebuild() }
        rebuild()
    }

    func becameActive() {
        link.refresh()
        rebuild()
    }

    /// recompute off the main thread; the newest request wins
    func rebuild() {
        generation += 1
        let gen = generation
        let records = store.snapshot()
        let p = profile
        let now = Date()
        let today = cal.day(of: now)
        let second = cal.secondOfDay(now)
        var live: Int?
        if let at = link.liveStepsAt, cal.day(of: at) == today { live = link.liveSteps }
        Task.detached(priority: .userInitiated) {
            let m = HealthModel.build(records: records, profile: p, today: today,
                                      nowSecond: second, liveSteps: live)
            await MainActor.run {
                if gen == self.generation { self.model = m }
            }
        }
    }

    private func scheduleRebuild() {
        rebuildTask?.cancel()
        rebuildTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(600))
            guard !Task.isCancelled else { return }
            self?.rebuild()
        }
    }

    func eraseHistory() {
        try? store.eraseAll()
        rebuild()
    }

    /// one row per day, for the share sheet
    func exportCSV() -> URL? {
        let m = model
        guard let first = m.firstDay else { return nil }
        var out = "date,steps,distance_km,calories,active_calories,active_minutes,resting_hr,avg_hr,min_hr,max_hr," +
            "fat_burn_min,cardio_min,peak_min,sleep_min,sleep_score,bedtime,wake,water_ml,workouts,workout_min\n"
        let df = DateFormatter()
        df.dateFormat = "yyyy-MM-dd"
        let tf = DateFormatter()
        tf.dateFormat = "HH:mm"
        for day in first...m.today {
            let d = m.summary(day)
            guard d.hasData else { continue }
            let s = d.mainSleep
            let cols: [String] = [
                df.string(from: cal.date(day)), "\(d.steps)", String(format: "%.2f", d.distanceKm),
                "\(d.caloriesTotal)", "\(d.caloriesActive)", "\(d.activeMinutes)",
                d.restingHR.map(String.init) ?? "", d.hrAvg.map(String.init) ?? "",
                d.hrMin.map(String.init) ?? "", d.hrMax.map(String.init) ?? "",
                "\(d.zoneMinutes[.fatBurn] ?? 0)", "\(d.zoneMinutes[.cardio] ?? 0)", "\(d.zoneMinutes[.peak] ?? 0)",
                "\(d.sleepMinutes)", s.map { "\($0.score)" } ?? "",
                s.map { tf.string(from: cal.date(watchTime: $0.bed)) } ?? "",
                s.map { tf.string(from: cal.date(watchTime: $0.wake)) } ?? "",
                "\(d.water)", "\(d.workouts.count)", "\(d.workoutMinutes)",
            ]
            out += cols.joined(separator: ",") + "\n"
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("VoidHealth-daily.csv")
        do {
            try out.write(to: url, atomically: true, encoding: .utf8)
            return url
        } catch {
            return nil
        }
    }
}
