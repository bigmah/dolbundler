//  A menu bar item and window for starting and stopping the driver.
//
//  The driver already prints everything worth showing, so this app does not add
//  any protocol of its own: it spawns `gc_controller`, reads its stdout, and
//  maps the lines it already emits onto UI state. Rust's stdout is a
//  `LineWriter`, so it stays line-buffered when piped and the log arrives
//  promptly without needing a pty.
//
//  Two lifecycle decisions worth knowing:
//
//  - A driver started elsewhere (start.sh, or a bare binary run) is *adopted*
//    rather than ignored. Without that, Start would happily spawn a second
//    driver fighting the first one for the controller. An adopted driver can be
//    stopped from here, but has no live log — its stdout belongs to whoever
//    started it.
//
//  - Quitting stops a driver this app started. That is not a preference: the
//    child's stdout is our pipe, so once this process exits the next `println!`
//    hits EPIPE and the driver dies anyway. Stopping it cleanly beats letting it
//    abort. A driver we merely adopted is left alone.

import AppKit
import SwiftUI

/// Diagnostic trace to stderr. Unbuffered, so a crash cannot swallow the last
/// line — which is exactly the line that matters when tracing a dead button.
func uiLog(_ message: String) {
    FileHandle.standardError.write("gcui: \(message)\n".data(using: .utf8)!)
}

// MARK: - Driver

@MainActor
final class Driver: ObservableObject {
    enum State: Equatable {
        case stopped
        case scanning
        case running
        /// Started outside this app; we can stop it, but cannot read its output.
        case external(pid_t)
        case failed(String)
    }

    @Published private(set) var state: State = .stopped
    @Published private(set) var transport = ""
    @Published private(set) var dolphinAttached = false
    @Published private(set) var rate: Int?
    @Published private(set) var log: [String] = []

    @Published var wantedTransport = "auto" {
        didSet {
            guard oldValue != wantedTransport, ownsProcess else { return }
            // The transport is chosen at startup, so applying it means a bounce.
            restartWanted = true
            stop()
        }
    }

    private var process: Process?
    private var pending = ""
    private var restartWanted = false
    private var poll: Timer?

    /// Keeps the log bounded; this is a status view, not a transcript.
    private static let logLimit = 300

    static let driverPath: String =
        (Bundle.main.object(forInfoDictionaryKey: "GCDriverPath") as? String) ?? ""

    var ownsProcess: Bool { process != nil }

    var isActive: Bool {
        switch state {
        case .running, .scanning, .external: return true
        case .stopped, .failed: return false
        }
    }

    init() {
        uiLog("Driver init — logging is live")
        adoptExternal()
        // Lets the menu bar label be inspected in its scanning state without a
        // click, which is the one thing a self-test otherwise cannot reach.
        if ProcessInfo.processInfo.environment["GC_UI_AUTOSTART"] != nil {
            DispatchQueue.main.async { [weak self] in self?.start() }
        }
        // Cheap, and the only way to notice a driver started from a terminal.
        poll = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.adoptExternal() }
        }
    }

    // MARK: Lifecycle

    func toggle() {
        uiLog("toggle() called, state=\(state) isActive=\(isActive)")
        isActive ? stop() : start()
    }

    func start() {
        uiLog("start() entered, state=\(state)")
        guard !isActive else {
            uiLog("start() bailed — already active")
            return
        }
        guard FileManager.default.isExecutableFile(atPath: Self.driverPath) else {
            state = .failed("driver not found at \(Self.driverPath) — run cargo build --release")
            return
        }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: Self.driverPath)
        // --stats is what produces the "N Hz" lines the UI shows as a rate.
        p.arguments = ["--transport", wantedTransport, "--stats"]

        let out = Pipe()
        p.standardOutput = out
        p.standardError = out
        out.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in self.ingest(text) }
        }
        p.terminationHandler = { _ in
            Task { @MainActor in self.childEnded() }
        }

        log.removeAll()
        append("$ gc_controller --transport \(wantedTransport) --stats")
        do {
            try p.run()
        } catch {
            state = .failed(error.localizedDescription)
            return
        }
        process = p
        state = .scanning
        uiLog("spawned pid \(p.processIdentifier)")
    }

    func stop() {
        if let p = process {
            // SIGINT, matching stop.sh and Ctrl-C: the driver handles it and
            // closes the pipe cleanly rather than being torn down mid-write.
            p.interrupt()
            return
        }
        if case .external(let pid) = state {
            kill(pid, SIGINT)
            // Give it a moment before re-checking, so the UI does not flicker
            // back to "running" on the next poll.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
                self?.adoptExternal()
            }
        }
    }

    private func childEnded() {
        process = nil
        pending = ""
        rate = nil
        dolphinAttached = false
        transport = ""
        if case .failed = state {} else { state = .stopped }

        if restartWanted {
            restartWanted = false
            start()
        } else {
            adoptExternal()
        }
    }

    /// Notices a driver running outside this app.
    private func adoptExternal() {
        guard process == nil else { return }
        if let pid = Self.findRunningDriver() {
            if state != .external(pid) {
                state = .external(pid)
                transport = "started outside this app"
            }
        } else if case .external = state {
            state = .stopped
            transport = ""
        }
    }

    private static func findRunningDriver() -> pid_t? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        p.arguments = ["-x", "gc_controller"]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard let text = String(data: data, encoding: .utf8) else { return nil }
        return text.split(separator: "\n")
            .compactMap { pid_t($0.trimmingCharacters(in: .whitespaces)) }
            .first
    }

    /// Stops a driver we own. Called on quit — see the note at the top.
    func stopIfOwned() {
        guard let p = process else { return }
        p.interrupt()
        // Bounded: never hold up quitting because the driver is wedged.
        let deadline = Date().addingTimeInterval(1.5)
        while p.isRunning && Date() < deadline {
            usleep(50_000)
        }
    }

    // MARK: Output

    private func ingest(_ chunk: String) {
        pending += chunk
        while let nl = pending.firstIndex(of: "\n") {
            let line = String(pending[pending.startIndex..<nl])
            pending = String(pending[pending.index(after: nl)...])
            handle(line)
        }
    }

    private func handle(_ raw: String) {
        let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !line.isEmpty else { return }
        append(line)

        if line.hasPrefix("connected over USB") {
            transport = "USB (wired)"
            state = .running
        } else if line.hasPrefix("connected over BLE") {
            transport = "Bluetooth"
            state = .running
        } else if line.hasPrefix("nothing on USB") {
            transport = "no cable — trying Bluetooth"
        } else if line.hasPrefix("scanning for a controller") {
            transport = "scanning — hold sync"
            state = .scanning
        } else if line == "Dolphin attached" {
            dolphinAttached = true
        } else if line == "Dolphin detached" {
            dolphinAttached = false
        } else if line.hasSuffix(" Hz"), let hz = Int(line.dropLast(3)) {
            // Bare "250 Hz" is the live meter. "peak report rate: 250 Hz" fails
            // this parse on purpose — it is a summary, printed after shutdown.
            rate = hz
            state = .running
        } else if line.lowercased().hasPrefix("error:") {
            state = .failed(String(line.dropFirst(6)).trimmingCharacters(in: .whitespaces))
        }
    }

    private func append(_ line: String) {
        log.append(line)
        if log.count > Self.logLimit {
            log.removeFirst(log.count - Self.logLimit)
        }
    }

    // MARK: Presentation

    var statusText: String {
        switch state {
        case .stopped: return "Stopped"
        case .scanning: return "Starting"
        case .running: return "Running"
        case .external: return "Running"
        case .failed: return "Failed"
        }
    }

    var detailText: String {
        switch state {
        case .failed(let why): return why
        case .stopped: return "not running"
        default:
            var parts: [String] = []
            if !transport.isEmpty { parts.append(transport) }
            if let rate { parts.append("\(rate) Hz") }
            if parts.isEmpty { parts.append("starting…") }
            return parts.joined(separator: " · ")
        }
    }

    var dolphinText: String {
        if case .external = state { return "log unavailable — started outside this app" }
        guard isActive else { return "" }
        return dolphinAttached ? "Dolphin attached" : "waiting for Dolphin"
    }

    /// Filled means the driver is up. Dolphin's state deliberately does *not*
    /// change the icon: the menu bar only has to answer "is it running", and
    /// overloading it with a second axis made stopped and running-but-waiting
    /// render identically.
    var symbolName: String {
        switch state {
        case .running, .external: return "gamecontroller.fill"
        case .scanning: return "gamecontroller.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .stopped: return "gamecontroller"
        }
    }

    /// Text shown *next to* the menu bar icon.
    ///
    /// Clicking a menu item closes the menu, so anything that happens after a
    /// click — a 20 s Bluetooth scan, or its failure — is invisible unless the
    /// user thinks to reopen it. Those two states get a word in the menu bar so
    /// Start never looks like it did nothing. Running and stopped stay bare;
    /// permanent text in the menu bar is noise.
    var menuBarHint: String? {
        switch state {
        case .scanning: return transport.contains("sync") ? "hold sync" : "starting"
        case .failed: return "failed"
        case .running, .external, .stopped: return nil
        }
    }

    var statusColor: Color {
        switch state {
        case .running, .external: return dolphinAttached ? .green : .yellow
        case .scanning: return .yellow
        case .failed: return .red
        case .stopped: return .secondary
        }
    }
}

// MARK: - Window

struct ContentView: View {
    @EnvironmentObject var driver: Driver

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top, spacing: 10) {
                Circle().fill(driver.statusColor).frame(width: 10, height: 10).padding(.top, 5)
                VStack(alignment: .leading, spacing: 2) {
                    Text(driver.statusText).font(.headline)
                    Text(driver.detailText).font(.subheadline).foregroundStyle(.secondary)
                    if !driver.dolphinText.isEmpty {
                        Text(driver.dolphinText).font(.subheadline).foregroundStyle(.secondary)
                    }
                }
                Spacer()
            }

            Picker("Transport", selection: $driver.wantedTransport) {
                Text("Auto").tag("auto")
                Text("USB").tag("usb")
                Text("Bluetooth").tag("ble")
            }
            .pickerStyle(.segmented)
            .disabled(!driver.ownsProcess && driver.isActive)

            Button(driver.isActive ? "Stop" : "Start") { driver.toggle() }
                .keyboardShortcut(driver.isActive ? "s" : .return)
                .frame(maxWidth: .infinity)
                .controlSize(.large)

            Divider()

            Text("Log").font(.caption).foregroundStyle(.secondary)
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(driver.log.enumerated()), id: \.offset) { i, line in
                            Text(line)
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(i)
                        }
                    }
                    .padding(6)
                }
                .background(Color(nsColor: .textBackgroundColor))
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .onChange(of: driver.log.count) { _, count in
                    withAnimation { proxy.scrollTo(count - 1, anchor: .bottom) }
                }
            }
        }
        .padding(20)
        .frame(minWidth: 380, minHeight: 420)
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate {
    var driver: Driver?

    /// `GC_UI_SELFTEST=1 …/GcController` exercises the exact path the Start
    /// button takes and prints what happened. Clicking a menu item cannot be
    /// automated, so without this there is no way to tell "the spawn failed"
    /// from "it spawned and the driver exited quietly".
    func applicationDidFinishLaunching(_ notification: Notification) {
        guard ProcessInfo.processInfo.environment["GC_UI_SELFTEST"] != nil else { return }
        MainActor.assumeIsolated {
            let d = Driver()
            note("driverPath = \(Driver.driverPath)")
            note("executable = \(FileManager.default.isExecutableFile(atPath: Driver.driverPath))")
            note("state before = \(d.state)")
            d.start()
            note("state after start() = \(d.state), ownsProcess = \(d.ownsProcess)")

            let secs = Double(ProcessInfo.processInfo.environment["GC_UI_SELFTEST"] ?? "") ?? 5
            DispatchQueue.main.asyncAfter(deadline: .now() + secs) { [self] in
                MainActor.assumeIsolated {
                    self.note("state after \(secs)s = \(d.state)")
                    self.note("menuBarHint = \(d.menuBarHint ?? "nil")")
                    self.note("child alive = \(d.ownsProcess)")
                    for line in d.log { self.note("log| \(line)") }
                    d.stopIfOwned()
                    exit(d.log.count > 1 ? 0 : 1)
                }
            }
        }
    }

    private func note(_ s: String) {
        FileHandle.standardError.write("selftest: \(s)\n".data(using: .utf8)!)
    }

    func applicationWillTerminate(_ notification: Notification) {
        MainActor.assumeIsolated { driver?.stopIfOwned() }
    }
}

@main
struct GcControllerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var driver = Driver()
    @Environment(\.openWindow) private var openWindow

    var body: some Scene {
        MenuBarExtra {
            Text("\(driver.statusText) — \(driver.detailText)")
            if !driver.dolphinText.isEmpty {
                Text(driver.dolphinText)
            }
            Divider()
            Button(driver.isActive ? "Stop" : "Start") {
                uiLog("menu button tapped")
                driver.toggle()
            }
            .keyboardShortcut("s")
            Divider()
            Picker("Transport", selection: $driver.wantedTransport) {
                Text("Auto").tag("auto")
                Text("USB").tag("usb")
                Text("Bluetooth").tag("ble")
            }
            Divider()
            Button("Show Window…") {
                openWindow(id: "main")
                NSApp.activate(ignoringOtherApps: true)
            }
            Button("Quit") { NSApp.terminate(nil) }
                .keyboardShortcut("q")
        } label: {
            if let hint = driver.menuBarHint {
                // Image + Text needs an explicit stack; a bare pair renders as
                // the image alone.
                HStack(spacing: 3) {
                    Image(systemName: driver.symbolName)
                    Text(hint)
                }
            } else {
                Image(systemName: driver.symbolName)
            }
        }

        Window("NSO GameCube Controller", id: "main") {
            ContentView()
                .environmentObject(driver)
                .onAppear { delegate.driver = driver }
        }
        .defaultSize(width: 400, height: 460)
    }
}
