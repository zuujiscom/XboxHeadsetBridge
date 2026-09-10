import Foundation
import Observation

/// Owns the `gip-bridge` daemon: starts it, stops it, and notices when a bridge
/// someone else launched (from a terminal, say) is already running.
@MainActor
@Observable
final class BridgeController {

    enum State: Equatable {
        case stopped
        /// Started by this app; we can stop it again.
        case running
        /// Running, but started outside this app (a terminal, a previous run).
        /// Still ours to stop: it is the same daemon either way.
        case runningExternally
        case failed(String)
    }

    private(set) var state: State = .stopped
    private(set) var status = HeadsetStatus()
    private(set) var hasRing = false

    private var process: Process?
    private var pollTimer: Timer?

    /// The bridge writes to a log rather than the terminal it no longer has.
    static let logURL: URL = {
        let logs = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs", isDirectory: true)
        return logs.appendingPathComponent("XboxHeadsetBridge.log")
    }()

    /// The daemon ships inside the app bundle; the source-tree path is a
    /// fallback so the app can be run straight out of `build/`.
    static var bridgeExecutableURL: URL? {
        if let bundled = Bundle.main.url(forResource: "gip-bridge", withExtension: nil) {
            return bundled
        }
        let sibling = Bundle.main.bundleURL
            .deletingLastPathComponent()
            .appendingPathComponent("gip-bridge")
        return FileManager.default.isExecutableFile(atPath: sibling.path) ? sibling : nil
    }

    init() {
        refresh()
        let timer = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        pollTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    // MARK: - Lifecycle

    func toggle() {
        switch state {
        case .running, .runningExternally:
            stop()
        case .stopped, .failed:
            start()
        }
    }

    func start() {
        guard process == nil else { return }

        guard let executable = Self.bridgeExecutableURL else {
            state = .failed("gip-bridge is missing from the app bundle")
            return
        }

        prepareLog()

        let task = Process()
        task.executableURL = executable
        // No --verbose: the twice-a-second counter line is terminal-only noise
        // and would otherwise grow the log without bound.
        task.arguments = []
        if let handle = try? FileHandle(forWritingTo: Self.logURL) {
            handle.seekToEndOfFile()
            task.standardOutput = handle
            task.standardError = handle
        }
        task.terminationHandler = { [weak self] finished in
            Task { @MainActor in
                self?.process = nil
                Self.ownedPID = 0
                // A non-zero exit that we did not ask for is worth surfacing;
                // SIGTERM from `stop()` is not.
                if finished.terminationReason == .uncaughtSignal || finished.terminationStatus == 0 {
                    self?.state = .stopped
                } else {
                    self?.state = .failed("gip-bridge exited with status \(finished.terminationStatus)")
                }
                self?.refresh()
            }
        }

        do {
            try task.run()
            process = task
            Self.ownedPID = task.processIdentifier
            state = .running
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    /// Stops the bridge whether or not this app started it. A daemon launched
    /// from a terminal is the same daemon; refusing to stop it just left the
    /// menu bar control inert whenever the bridge had been started any other
    /// way, which is the common case.
    func stop() {
        // SIGINT, not SIGKILL: the bridge's handler unwinds the USB transfers
        // and closes the interface, which SIGKILL would leave dangling.
        if let task = process {
            kill(task.processIdentifier, SIGINT)
            process = nil
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                if task.isRunning { task.terminate() }
            }
        }
        for pid in Self.externalBridgePIDs() {
            kill(pid, SIGINT)
        }
        state = .stopped
    }

    /// Called from the app delegate: a child process outlives its parent unless
    /// somebody stops it.
    func stopIfOwned() {
        guard let task = process, task.isRunning else { return }
        kill(task.processIdentifier, SIGINT)
        task.waitUntilExit()
    }

    // MARK: - Polling

    private func refresh() {
        if process == nil {
            if Self.externalBridgeIsRunning() {
                state = .runningExternally
            } else if case .failed = state {
                // Keep the error on screen until the user tries again.
            } else {
                state = .stopped
            }
        }

        if let snapshot = HeadsetStatus.read() {
            hasRing = true
            status = snapshot
        } else {
            hasRing = false
            status = HeadsetStatus()
        }
    }

    private static func externalBridgeIsRunning() -> Bool {
        !externalBridgePIDs().isEmpty
    }

    /// PIDs of any gip-bridge this app did not spawn.
    private static func externalBridgePIDs() -> [pid_t] {
        let task = Process()
        let pipe = Pipe()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        task.arguments = ["-x", "gip-bridge"]
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            task.waitUntilExit()
            let mine = self.ownedPID
            return String(decoding: data, as: UTF8.self)
                .split(whereSeparator: \.isNewline)
                .compactMap { pid_t($0.trimmingCharacters(in: .whitespaces)) }
                .filter { $0 != mine }
        } catch {
            return []
        }
    }

    /// Set while this app owns a child, so it is not also counted as external.
    nonisolated(unsafe) private static var ownedPID: pid_t = 0

    private func prepareLog() {
        let fm = FileManager.default
        let url = Self.logURL
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)

        // Start each session from a bounded file: the handshake logging is
        // small, but an auto-reconnect loop can run for days.
        if let size = try? fm.attributesOfItem(atPath: url.path)[.size] as? UInt64, size > 1_000_000 {
            try? fm.removeItem(at: url)
        }
        if !fm.fileExists(atPath: url.path) {
            fm.createFile(atPath: url.path, contents: nil)
        }
    }
}
