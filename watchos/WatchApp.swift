import SwiftUI
import Combine
import WatchKit

@MainActor
final class WatchRenderer: ObservableObject {
    @Published var image: CGImage?
    @Published var error = ""
    @Published var started = false
    private var lastFrame: UInt64 = 0

    let games: [URL] = {
        let bundled = Bundle.main.bundleURL.appendingPathComponent("Games", isDirectory: true)
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Games", isDirectory: true)
        return [documents, bundled].flatMap {
            (try? FileManager.default.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil)) ?? []
        }.filter { $0.pathExtension.lowercased() == "wad" }.sorted {
            $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending
        }
    }()

    func start(_ game: URL) {
        guard !started else { return }
        started = true
        // The engine renders at the panel's native pixel width.
        let device = WKInterfaceDevice.current()
        GZDWatchSetScreen(Int32(device.screenBounds.width * device.screenScale),
                          Int32(device.screenBounds.height * device.screenScale))
        game.path.withCString { GZDWatchStart($0) }
    }

    func refresh() {
        guard started else { return }
        let count = GZDWatchFrameCount()
        if count != lastFrame, let next = GZDWatchCopyImage() {
            image = next
            lastFrame = count
        }
        let message = String(cString: GZDWatchError())
        if message != error { error = message }
    }
}

@main
struct GZDoomWatchApp: App {
    @StateObject private var renderer = WatchRenderer()
    @Environment(\.scenePhase) private var scenePhase
    private let frames = Timer.publish(every: 1.0 / 35.0, on: .main, in: .common).autoconnect()

    var body: some Scene {
        WindowGroup {
            ZStack {
                Color.black.ignoresSafeArea()
                if !renderer.error.isEmpty {
                    ScrollView { Text(renderer.error).font(.caption).padding() }
                } else if let image = renderer.image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .interpolation(.medium)
                        .aspectRatio(4.0 / 3.0, contentMode: .fit)
                        .ignoresSafeArea()
                } else if renderer.started {
                    ProgressView("Starting GZDoom…")
                } else {
                    List {
                        Text("GZDoom").font(.headline)
                        if renderer.games.isEmpty {
                            Text("No games available.")
                                .font(.caption)
                        }
                        ForEach(renderer.games, id: \.path) { game in
                            Button(game.deletingPathExtension().lastPathComponent) {
                                renderer.start(game)
                            }
                        }
                        Text("Choose a game to view its starting level.")
                            .font(.caption2)
                    }
                }
            }
            .onReceive(frames) { _ in
                if scenePhase == .active { renderer.refresh() }
            }
            .onChange(of: scenePhase) { _, phase in
                GZDWatchSetActive(phase == .active)
            }
            .task {
                GZDWatchSetActive(scenePhase == .active)
                if ProcessInfo.processInfo.arguments.contains("-watch-autostart"), let game = renderer.games.first {
                    renderer.start(game)
                }
            }
        }
    }
}
