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

    /// The battery reading is the whole point of the menu bar item, so it is
    /// what the icon shows whenever the headset has reported one.
    private var menuBarSymbol: String {
        guard bridge.state != .stopped, bridge.status.isOnline else {
            return "headphones"
        }
        return bridge.status.battery?.symbolName ?? "headphones"
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
                Image(systemName: bridge.status.battery?.symbolName ?? "battery.0percent")
                    .font(.system(size: 15))
                    .foregroundStyle(batteryTint)
                Text("Battery")
                    .font(.callout)
                Spacer()
                Text(batteryText)
                    .font(.callout.weight(.medium))
                    .foregroundStyle(batteryTint)
            }

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
            LabeledMeter(title: "Chat", value: bridge.status.inputVolume)
        }
    }

    private var controls: some View {
        VStack(spacing: 6) {
            Button(bridge.toggleTitle) { bridge.toggle() }
                .buttonStyle(.borderedProminent)
                .controlSize(.regular)
                .frame(maxWidth: .infinity)
                .disabled(bridge.state == .runningExternally)

            HStack {
                Button("Open Log") {
                    NSWorkspace.shared.open(BridgeController.logURL)
                }
                .buttonStyle(.link)
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                    .buttonStyle(.link)
            }
            .font(.caption)
        }
    }

    private var batteryText: String {
        guard bridge.status.isOnline else { return "—" }
        guard let battery = bridge.status.battery else { return "Waiting…" }
        return battery.label
    }

    private var batteryTint: Color {
        guard let battery = bridge.status.battery, bridge.status.isOnline else { return .secondary }
        return battery.isLow ? .orange : .green
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
        case .running:           return "Stop Bridge"
        case .runningExternally: return "Running in Terminal"
        case .stopped, .failed:  return "Start Bridge"
        }
    }

    var stateDescription: String {
        switch state {
        case .stopped:           return "Bridge stopped"
        case .running:           return status.isOnline ? "Connected" : "Starting…"
        case .runningExternally: return "Running outside this app"
        case .failed:            return "Bridge failed"
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
