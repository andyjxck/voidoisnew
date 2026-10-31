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
            guard profile != oldValue else { return }
            if let data = try? JSONEncoder().encode(profile) {
                UserDefaults.standard.set(data, forKey: Self.profileKey)
            }
            rebuild()
        }
    }
    private(set) var model: HealthModel
    private(set) var recordCount = 0
    private(set) var storeError: String?
    let cal = DayCalendar()
    let store: RecordStore
    let link: WatchLink

    @ObservationIgnored private var rebuildTask: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    private static let profileKey = "profile"

    init() {
        var p = Profile()
        if let data = UserDefaults.standard.data(forKey: Self.profileKey),
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
            // keep the app usable; nothing synced into this copy is acknowledged
            // unless it reaches disk, so the watch still holds it
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
                if gen == self.generation {
                    self.model = m
                    self.recordCount = records.count
                }
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

    /// wipes the phone's copy; records the watch has already erased are gone
    func eraseHistory() {
        do {
            try store.eraseAll()
            storeError = nil
        } catch {
            storeError = "Could not erase the history: \(error.localizedDescription)"
        }
        rebuild()
    }

    private static func posix(_ format: String) -> DateFormatter {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = format
        return f
    }

    private func write(_ text: String, name: String) -> URL? {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            return url
        } catch {
            return nil
        }
    }

    /// one row per day, for the share sheet
    func exportDailyCSV() -> URL? {
        let m = model
        guard let first = m.firstDay, first <= m.today else { return nil }
        var out = "date,steps,distance_km,calories,active_calories,active_minutes,resting_hr,avg_hr,min_hr,max_hr," +
            "fat_burn_min,cardio_min,peak_min,sleep_min,sleep_score,bedtime,wake,restless_min,water_ml,workouts,workout_min\n"
        let df = Self.posix("yyyy-MM-dd")
        let tf = Self.posix("HH:mm")
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
                s.map { "\($0.restlessMinutes)" } ?? "",
                "\(d.water)", "\(d.workouts.count)", "\(d.workoutMinutes)",
            ]
            out += cols.joined(separator: ",") + "\n"
        }
        return write(out, name: "VoidHealth-daily.csv")
    }

    /// every record exactly as the watch logged it, one row each
    func exportRecordsCSV() -> URL? {
        let records = store.snapshot().sorted { $0.t != $1.t ? $0.t < $1.t : $0.seq < $1.seq }
        guard !records.isEmpty else { return nil }
        let tf = Self.posix("yyyy-MM-dd HH:mm:ss")
        var out = "time,type,steps,avg_hr,max_hr,battery,charging,moving_s,water_delta_ml,water_total_ml," +
            "active_s,sets,routine,seconds_in_bed,clock_guess\n"
        out.reserveCapacity(records.count * 48)
        for r in records {
            var c = [String](repeating: "", count: 15)
            c[0] = tf.string(from: cal.date(watchTime: r.t))
            c[14] = r.clockGuess ? "1" : "0"
            switch r.kind {
            case .sample:
                c[1] = "sample"; c[2] = "\(r.steps)"; c[3] = "\(r.avgBPM)"; c[4] = "\(r.maxBPM)"
                c[5] = "\(r.battery)"; c[6] = r.charging ? "1" : "0"; c[7] = "\(r.movingSeconds)"
            case .water:
                c[1] = "water"; c[8] = "\(r.waterDelta)"; c[9] = "\(r.waterTotal)"
            case .workout:
                c[1] = "workout"; c[3] = "\(r.workoutAvgBPM)"; c[4] = "\(r.workoutMaxBPM)"
                c[10] = "\(r.activeSeconds)"; c[11] = "\(r.sets)"
                c[12] = r.routine.map { ["push", "pull", "legs"].indices.contains($0) ? ["push", "pull", "legs"][$0] : "\($0)" } ?? "quick"
            case .sleep:
                c[1] = "sleep"; c[13] = "\(r.secondsInBed)"
            case .day:
                c[1] = "day"; c[2] = "\(r.daySteps)"; c[3] = r.dayRestBPM > 0 ? "\(r.dayRestBPM)" : ""
                c[9] = "\(r.dayWater)"
            case nil:
                continue
            }
            out += c.joined(separator: ",") + "\n"
        }
        return write(out, name: "VoidHealth-records.csv")
    }
}
