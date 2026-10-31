import SwiftUI

@main
struct VoidHealthApp: App {
    @State private var app = AppModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(app)
                .preferredColorScheme(.dark)
                .tint(.teal)
        }
        .onChange(of: phase) { _, now in
            if now == .active { app.becameActive() }
        }
    }
}

struct RootView: View {
    var body: some View {
        TabView {
            NavigationStack { TodayView() }
                .tabItem { Label("Today", systemImage: "sun.max.fill") }
            NavigationStack { TrendsView() }
                .tabItem { Label("Trends", systemImage: "chart.xyaxis.line") }
            NavigationStack { SleepView() }
                .tabItem { Label("Sleep", systemImage: "moon.stars.fill") }
            NavigationStack { WorkoutsView() }
                .tabItem { Label("Workouts", systemImage: "dumbbell.fill") }
            NavigationStack { YouView() }
                .tabItem { Label("You", systemImage: "person.crop.circle") }
        }
    }
}
