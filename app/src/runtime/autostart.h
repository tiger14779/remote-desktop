#pragma once

#include <QString>

// Windows logon-time auto-start via HKCU\Software\Microsoft\Windows\
// CurrentVersion\Run. Deliberately NOT a real SCM Windows service because:
//
//   * A session-0 service can't capture the interactive desktop (WGC/DXGI
//     needs the active session), so a "true" service would need a helper
//     process to be spawned into the console session anyway.
//   * HKCU Run needs no elevation, so the checkbox works without a UAC
//     prompt for a normal user install.
//   * Same mechanism RustDesk / TeamViewer use for user-level auto-start.
//
// If we later want per-machine service mode (starts before user logon,
// works with locked screens), that's a separate installer flow — this API
// stays as the "just this user, at their logon" path.
namespace Autostart {

// Registry value name under Run. Change carefully — old value stays behind
// if the name is renamed without cleanup.
constexpr const char* kAppName = "RemoteDesktop";

// Is the current exe registered to run at logon?
bool isEnabled();

// Register the current process's exe path under Run. Returns true on
// success. Overwrites an existing value with the current exe path (handy
// when the exe moved between installs).
bool enable();

// Remove the Run entry. Returns true on success (or if the entry wasn't
// there to begin with).
bool disable();

} // namespace Autostart
