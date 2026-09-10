import Foundation

/// A snapshot of the headset, as reported by the bridge through the shared ring.
struct HeadsetStatus: Equatable {
    enum BatteryLevel: UInt32 {
        case empty = 0, low = 1, medium = 2, full = 3
    }

    /// Mirrors `enum gip_battery_type`. A wired/charging headset reports `none`.
    enum BatteryType: UInt32 {
        case none = 0, standard = 1, rechargeable = 2
    }

    var isOnline = false
    var isMicMuted = false
    /// The headset's own volume dial, as the bridge reads it off GIP.
    var headsetVolume: Int = 0
    var inputVolume: Int = 0
    /// macOS-side volume applied by the HAL plug-in — this is what the keyboard
    /// volume keys change.
    var systemVolume: Int = 100
    var isSystemMuted = false
    /// `nil` until the headset has actually sent a status packet — a raw level
    /// of 0 means "empty", which is not the same as "not reported yet".
    var battery: BatteryLevel?
    var batteryType: BatteryType = .none
    var playbackFrames: UInt64 = 0
    var captureFrames: UInt64 = 0

    static func read() -> HeadsetStatus? {
        guard ring_status_open() else { return nil }

        var raw = ring_status_t()
        guard ring_status_read(&raw) else { return nil }

        var status = HeadsetStatus()
        status.isOnline = raw.online != 0
        status.isMicMuted = raw.mic_muted != 0
        status.headsetVolume = Int(raw.vol_out)
        status.inputVolume = Int(raw.vol_in)
        status.systemVolume = Int(raw.host_vol_out)
        status.isSystemMuted = raw.host_muted != 0
        status.batteryType = BatteryType(rawValue: raw.battery_type) ?? .none
        if raw.battery_seen != 0 {
            status.battery = BatteryLevel(rawValue: raw.battery_level)
        }
        status.playbackFrames = raw.out_write_frames
        status.captureFrames = raw.in_write_frames
        return status
    }
}

extension HeadsetStatus.BatteryLevel {
    var label: String {
        switch self {
        case .empty:  return "Critical"
        case .low:    return "Low"
        case .medium: return "Medium"
        case .full:   return "Full"
        }
    }

    /// SF Symbol matching the level. The menu bar shows this directly, so it is
    /// the one thing that has to read correctly at 16pt.
    var symbolName: String {
        switch self {
        case .empty:  return "battery.0percent"
        case .low:    return "battery.25percent"
        case .medium: return "battery.50percent"
        case .full:   return "battery.100percent"
        }
    }

    var isLow: Bool { self == .empty || self == .low }
}
