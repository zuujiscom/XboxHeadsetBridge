import Foundation
import Observation

/// Runs the bridge. The bridge core is linked into this app and runs on its own
/// thread rather than as a child process, so there is nothing to spawn, adopt or
/// leave orphaned. The standalone `gip-bridge` CLI still exists for diagnosis,
/// and holds the USB device exclusively while it runs — hence the check for one.
@MainActor
@Observable
final class BridgeController {

    enum State: Equatable {
        case stopped
        /// Running on this app's bridge thread.
        case running
        /// The standalone CLI has the device. It is a separate process holding
        /// exclusive USB access, so this app cannot run its own bridge until it
        /// exits — and must not kill someone's debugging session.
        case runningExternally
        case failed(String)
    }

    private(set) var state: State = .stopped
    private(set) var status = HeadsetStatus()
    private(set) var hasRing = false

    private var pollTimer: Timer?

    /// The bridge core still logs to stdout, which a menu bar app does not
    /// have, so stdout and stderr are redirected here for the life of the app.
    static let logURL: URL = {
        let logs = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs", isDirectory: true)
        return logs.appendingPathComponent("XboxHeadsetBridge.log")
    }()

    /// The bridge core is a single process-wide instance, so this controller is
    /// too — the bridge thread needs a way back to it.
    nonisolated(unsafe) static var shared: BridgeController?

    init() {
        Self.shared = self
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
        case .running:
            stop()
        case .runningExternally:
            // A terminal session owns the device; leaving it alone is the only
            // correct move, since we cannot open the device anyway.
            break
        case .stopped, .failed:
            start()
        }
    }

    func start() {
        guard state != .running else { return }

        if Self.externalBridgeIsRunning() {
            state = .runningExternally
            return
        }

        redirectOutputToLogOnce()

        state = .running
        // bridge_run blocks until bridge_stop, and its IOKit event sources bind
        // to CFRunLoopGetCurrent(), so it needs a thread of its own that it can
        // keep for the whole session.
        let thread = Thread {
            bridge_set_verbose(false)
            let rc = bridge_run()
            Task { @MainActor in
                BridgeController.shared?.bridgeThreadFinished(rc: rc)
            }
        }
        thread.name = "gip-bridge"
        thread.stackSize = 512 * 1024
        thread.start()
    }

    func stop() {
        guard bridge_is_running() else {
            state = .stopped
            return
        }
        bridge_stop()
        // bridge_run polls its run loop, so it returns shortly; the thread's
        // completion handler moves us to .stopped.
    }

    /// Called from the app delegate: the bridge thread would otherwise keep the
    /// USB interface open past termination.
    func stopIfOwned() {
        guard bridge_is_running() else { return }
        bridge_stop()
        // Give the session a moment to unwind its transfers and close the
        // interface, the same courtesy SIGINT gave the child process.
        for _ in 0..<40 where bridge_is_running() {
            Thread.sleep(forTimeInterval: 0.05)
        }
    }

    private func bridgeThreadFinished(rc: Int32) {
        state = rc == 0 ? .stopped : .failed("the bridge could not start (\(rc))")
        refresh()
    }

    // MARK: - Polling

    private func refresh() {
        if !bridge_is_running() {
            if Self.externalBridgeIsRunning() {
                state = .runningExternally
            } else if case .failed = state {
                // Keep the error on screen until the user tries again.
            } else {
                state = .stopped
            }
        } else if state != .running {
            state = .running
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

    /// PIDs of any standalone gip-bridge process.
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
            return String(decoding: data, as: UTF8.self)
                .split(whereSeparator: \.isNewline)
                .compactMap { pid_t($0.trimmingCharacters(in: .whitespaces)) }
        } catch {
            return []
        }
    }



    /// Point stdout/stderr at the log file. Done once: reopening while the
    /// bridge thread is mid-write would race it.
    private static var outputRedirected = false

    private func redirectOutputToLogOnce() {
        guard !Self.outputRedirected else { return }
        Self.outputRedirected = true
        prepareLog()
        Self.logURL.path.withCString { path in
            _ = freopen(path, "a", stdout)
            _ = freopen(path, "a", stderr)
        }
        setvbuf(stdout, nil, _IONBF, 0)
    }

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
