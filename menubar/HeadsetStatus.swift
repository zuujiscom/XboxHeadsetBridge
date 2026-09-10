import Foundation

/// A snapshot of the headset, as reported by the bridge through the shared ring.
struct HeadsetStatus: Equatable {
    var isOnline = false
    var isMicMuted = false
    /// The headset's two physical dials. Both are applied by the headset
    /// itself, so these are reported for display only, and are `nil` until a
    /// volume packet arrives. Confirmed by sweeping each dial in isolation:
    /// the main dial moves p[2], the chat dial moves p[3].
    var headsetDial: Int?
    var chatDial: Int?
    /// macOS-side volume applied by the HAL plug-in — this is what the keyboard
    /// volume keys change.
    var systemVolume: Int = 100
    var isSystemMuted = false
    var playbackFrames: UInt64 = 0
    var captureFrames: UInt64 = 0

    static func read() -> HeadsetStatus? {
        guard ring_status_open() else { return nil }

        var raw = ring_status_t()
        guard ring_status_read(&raw) else { return nil }

        var status = HeadsetStatus()
        status.isOnline = raw.online != 0
        status.isMicMuted = raw.mic_muted != 0
        if raw.vol_seen != 0 {
            status.headsetDial = Int(raw.gain_out)
            status.chatDial = Int(raw.vol_out)
        }
        status.systemVolume = Int(raw.host_vol_out)
        status.isSystemMuted = raw.host_muted != 0
        status.playbackFrames = raw.out_write_frames
        status.captureFrames = raw.in_write_frames
        return status
    }
}
