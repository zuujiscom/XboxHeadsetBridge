import Foundation
import AppKit
import IOKit
import IOKit.usb
import CoreAudio

/// A one-shot report for when something is wrong.
///
/// The checks are the failure modes this project has actually produced, in the
/// order they break: the dongle vanishing from USB, the HAL plug-in being
/// missing or stale against the running build, coreaudiod not publishing the
/// device, the standalone CLI holding the device exclusively, and the bridge
/// failing its GIP authentication. Each line is meant to be readable by someone
/// who does not know the codebase.
@MainActor
enum Diagnostics {

    static func report(_ bridge: BridgeController) -> String {
        var out: [String] = []
        func line(_ label: String, _ value: String, ok: Bool? = nil) {
            let mark = ok.map { $0 ? "OK  " : "FAIL" } ?? "--  "
            out.append("\(mark) \(label.padding(toLength: 24, withPad: " ", startingAt: 0)) \(value)")
        }

        out.append("XboxHeadsetBridge diagnostics — \(Self.timestamp)")
        out.append(String(repeating: "-", count: 64))

        // 1. Hardware
        let usb = dongleIsPresent
        line("USB dongle 0e6f:0234", usb ? "present" : "NOT FOUND — replug it", ok: usb)

        // 2. Virtual audio device
        let dev = audioDevice
        line("Virtual audio device", dev ?? "NOT PUBLISHED — is the plug-in installed?", ok: dev != nil)
        line("Default output", defaultOutputName ?? "unknown")

        // 3. HAL plug-in
        let installed = FileManager.default.fileExists(atPath: Self.installedPlugin)
        line("HAL plug-in installed", installed ? Self.installedPlugin : "missing", ok: installed)
        if let stale = pluginIsStale {
            line("Plug-in matches build", stale ? "STALE — sudo make install-plugin" : "current", ok: !stale)
        }
        line("coreaudiod", processExists("coreaudiod") ? "running" : "not running",
             ok: processExists("coreaudiod"))

        // 4. Bridge
        line("Bridge", bridge.stateDescription, ok: bridge.isActive)
        if case .failed(let message) = bridge.state {
            line("Last bridge error", message, ok: false)
        }
        let cli = processExists("gip-bridge")
        if cli {
            line("Standalone CLI", "running — it holds the USB device exclusively", ok: false)
        }

        // 5. Shared ring
        if bridge.hasRing {
            let s = bridge.status
            line("Ring", "mapped")
            line("Headset streaming", s.isOnline ? "yes" : "no", ok: s.isOnline)
            line("Microphone", s.isMicMuted ? "muted" : "live")
            line("macOS volume", "\(s.systemVolume)%\(s.isSystemMuted ? " (muted)" : "")")
            line("Headset dial", s.headsetDial.map { "\($0)%" } ?? "not reported")
            line("Chat dial", s.chatDial.map { "\($0)%" } ?? "not reported")
            line("Playback frames", "\(s.playbackFrames)")
            line("Capture frames", "\(s.captureFrames)")
        } else {
            line("Ring", "NOT MAPPED — no bridge has run yet", ok: false)
        }

        // 6. App
        line("Open at Login", LoginItem.statusDescription)
        line("App location", Bundle.main.bundleURL.path)
        if !LoginItem.isInApplications {
            line("", "not in /Applications — a login item here breaks on rebuild")
        }

        out.append("")
        out.append("Last 40 log lines (\(BridgeController.logURL.path)):")
        out.append(String(repeating: "-", count: 64))
        out.append(tailLog(lines: 40))
        return out.joined(separator: "\n")
    }

    static func copyToPasteboard(_ bridge: BridgeController) {
        let text = report(bridge)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    // MARK: - Probes

    private static let installedPlugin =
        "/Library/Audio/Plug-Ins/HAL/XboxHeadset.driver/Contents/MacOS/XboxHeadset"

    private static var timestamp: String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f.string(from: Date())
    }

    /// Matches the bridge's own device lookup, so "present" here means the
    /// bridge should be able to open it.
    private static var dongleIsPresent: Bool {
        guard let matching = IOServiceMatching(kIOUSBDeviceClassName) else { return false }
        let dict = matching as NSMutableDictionary
        dict[kUSBVendorID] = 0x0e6f
        dict[kUSBProductID] = 0x0234

        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, dict, &iterator) == KERN_SUCCESS else {
            return false
        }
        defer { IOObjectRelease(iterator) }
        let service = IOIteratorNext(iterator)
        guard service != 0 else { return false }
        IOObjectRelease(service)
        return true
    }

    /// nil when the installed plug-in cannot be compared (no local build to
    /// compare against), rather than guessing.
    private static var pluginIsStale: Bool? {
        let built = Bundle.main.bundleURL
            .deletingLastPathComponent()
            .appendingPathComponent("XboxHeadset.driver/Contents/MacOS/XboxHeadset")
        guard let a = try? Data(contentsOf: built),
              let b = try? Data(contentsOf: URL(fileURLWithPath: installedPlugin)) else { return nil }
        return a != b
    }

    /// Name of the bridge's virtual device if coreaudiod is publishing it,
    /// looked up by the UID the plug-in reports rather than by name.
    private static var audioDevice: String? {
        for id in allDeviceIDs() where deviceString(id, kAudioDevicePropertyDeviceUID)
            == "XboxHeadsetBridge:Device" {
            return deviceString(id, kAudioDevicePropertyDeviceNameCFString) ?? "present"
        }
        return nil
    }

    private static var defaultOutputName: String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var id = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                         &address, 0, nil, &size, &id) == noErr,
              id != 0 else { return nil }
        return deviceString(id, kAudioDevicePropertyDeviceNameCFString)
    }

    private static func allDeviceIDs() -> [AudioDeviceID] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                             &address, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard !ids.isEmpty,
              AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                         &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }

    private static func deviceString(_ id: AudioDeviceID,
                                     _ selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: CFString = "" as CFString
        var size = UInt32(MemoryLayout<CFString>.size)
        guard AudioObjectGetPropertyData(id, &address, 0, nil, &size, &value) == noErr else {
            return nil
        }
        return value as String
    }

    private static func processExists(_ name: String) -> Bool {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        task.arguments = ["-x", name]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
            task.waitUntilExit()
            return task.terminationStatus == 0
        } catch {
            return false
        }
    }

    private static func tailLog(lines: Int) -> String {
        guard let data = try? Data(contentsOf: BridgeController.logURL),
              let text = String(data: data, encoding: .utf8) else {
            return "(no log yet)"
        }
        // The bridge redraws its counter line with \r; split on both so the
        // tail is not one enormous line.
        let all = text.split(whereSeparator: { $0 == "\n" || $0 == "\r" })
        return all.suffix(lines).joined(separator: "\n")
    }
}
