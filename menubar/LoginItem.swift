import Foundation
import ServiceManagement

/// "Open at Login", backed by `SMAppService` (macOS 13+) rather than a
/// hand-installed LaunchAgent plist: the registration follows the app bundle,
/// and the user can revoke it from System Settings > General > Login Items,
/// which they cannot do for a plist we dropped on disk ourselves.
enum LoginItem {

    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    /// `SMAppService` registers the bundle *at its current path*. Running out of
    /// `build/` therefore registers a path that a `make clean` will delete, so
    /// the app warns rather than silently creating a broken login item.
    static var isInApplications: Bool {
        Bundle.main.bundleURL.path.hasPrefix("/Applications/")
    }

    /// Returns nil on success, or a message worth showing the user.
    @discardableResult
    static func setEnabled(_ enabled: Bool) -> String? {
        do {
            if enabled {
                // Re-registering an already-registered app throws; treat the
                // desired end state as the goal, not the transition.
                guard SMAppService.mainApp.status != .enabled else { return nil }
                try SMAppService.mainApp.register()
            } else {
                guard SMAppService.mainApp.status == .enabled else { return nil }
                try SMAppService.mainApp.unregister()
            }
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    static var statusDescription: String {
        switch SMAppService.mainApp.status {
        case .enabled:        return "enabled"
        case .notRegistered:  return "not registered"
        case .notFound:       return "not found"
        case .requiresApproval:
            return "waiting for approval in System Settings > Login Items"
        @unknown default:     return "unknown"
        }
    }
}
