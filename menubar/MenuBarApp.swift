import SwiftUI
import AppKit

@main
struct XboxHeadsetMenuApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var bridge = BridgeController()

    var body: some Scene {
        MenuBarExtra {
            StatusPanel(bridge: bridge)
                .frame(width: 260)
        } label: {
            Image(systemName: menuBarSymbol)
                .onAppear { delegate.bridge = bridge }
        }
        .menuBarExtraStyle(.window)
    }

    /// This dongle never reports battery, so the icon just reflects whether the
    /// headset is connected.
    private var menuBarSymbol: String {
        bridge.isActive && bridge.status.isOnline
            ? "headphones" : "headphones.slash"
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var bridge: BridgeController?

    func applicationWillTerminate(_ notification: Notification) {
        // The daemon is a child process; without this it outlives the app and
        // keeps the USB interface open.
        MainActor.assumeIsolated { bridge?.stopIfOwned() }
    }
}

// MARK: - Panel

struct StatusPanel: View {
    @Bindable var bridge: BridgeController

    @State private var loginItemEnabled = LoginItem.isEnabled
    @State private var loginItemError: String?
    @State private var copiedDiagnostics = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header

            Divider()

            if bridge.isActive {
                readings
            } else {
                Text("The bridge is not running.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            if case .failed(let message) = bridge.state {
                Label(message, systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider()

            controls
        }
        .padding(12)
    }

    private var header: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(bridge.indicatorColor)
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 1) {
                Text("Xbox Wireless Headset")
                    .font(.system(size: 13, weight: .semibold))
                Text(bridge.stateDescription)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    private var readings: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: bridge.status.isMicMuted ? "mic.slash.fill" : "mic.fill")
                    .font(.system(size: 13))
                    .foregroundStyle(bridge.status.isMicMuted ? .orange : .secondary)
                Text("Microphone")
                    .font(.callout)
                Spacer()
                Text(bridge.status.isMicMuted ? "Muted" : "Live")
                    .font(.callout.weight(.medium))
                    .foregroundStyle(bridge.status.isMicMuted ? .orange : .primary)
            }

            LabeledMeter(
                title: bridge.status.isSystemMuted ? "Volume (muted)" : "Volume",
                value: bridge.status.systemVolume
            )
            if let dial = bridge.status.headsetDial {
                LabeledMeter(title: "Headset Dial", value: dial)
            }
            if let chat = bridge.status.chatDial {
                LabeledMeter(title: "Chat Dial", value: chat)
            }
        }
    }

    private var controls: some View {
        VStack(spacing: 6) {
            Button(bridge.toggleTitle) { bridge.toggle() }
                .buttonStyle(.borderedProminent)
                .controlSize(.regular)
                .frame(maxWidth: .infinity)

            Toggle("Start bridge when app opens", isOn: Binding(
                get: { bridge.startsAutomatically },
                set: { bridge.startsAutomatically = $0 }
            ))
            .toggleStyle(.checkbox)
            .font(.caption)

            Toggle("Open at Login", isOn: Binding(
                get: { loginItemEnabled },
                set: { newValue in
                    loginItemError = LoginItem.setEnabled(newValue)
                    loginItemEnabled = LoginItem.isEnabled
                }
            ))
            .toggleStyle(.checkbox)
            .font(.caption)

            if let loginItemError {
                Text(loginItemError)
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            } else if loginItemEnabled && !LoginItem.isInApplications {
                // A login item pointing into build/ breaks on the next clean.
                Text("Running from outside /Applications — use `make install-menubar`.")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack {
                Button("Open Log") {
                    NSWorkspace.shared.open(BridgeController.logURL)
                }
                .buttonStyle(.link)
                Button("Copy Diagnostics") {
                    Diagnostics.copyToPasteboard(bridge)
                    copiedDiagnostics = true
                }
                .buttonStyle(.link)
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                    .buttonStyle(.link)
            }
            .font(.caption)

            if copiedDiagnostics {
                Text("Diagnostics copied to the clipboard.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

}

private struct LabeledMeter: View {
    let title: String
    let value: Int

    var body: some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.callout)
            Spacer()
            ProgressView(value: Double(min(max(value, 0), 100)), total: 100)
                .frame(width: 90)
            Text("\(value)%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(width: 34, alignment: .trailing)
        }
    }
}

// MARK: - Presentation helpers

extension BridgeController {
    var isActive: Bool {
        state == .running || state == .runningExternally
    }

    var toggleTitle: String {
        switch state {
        case .running, .runningExternally: return "Stop Bridge"
        case .stopped, .failed:            return "Start Bridge"
        }
    }

    var stateDescription: String {
        switch state {
        case .stopped:
            return "Bridge stopped"
        case .running:
            return status.isOnline ? "Connected" : "Starting…"
        case .runningExternally:
            // Still controllable; the provenance is a detail, not a blocker.
            return status.isOnline ? "Connected (started elsewhere)" : "Starting…"
        case .failed:
            return "Bridge failed"
        }
    }

    var indicatorColor: Color {
        switch state {
        case .running, .runningExternally:
            return status.isOnline ? .green : .yellow
        case .failed:
            return .red
        case .stopped:
            return .secondary
        }
    }
}
