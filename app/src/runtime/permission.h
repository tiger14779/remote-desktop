#pragma once

// Hidden "can I remote-control other machines" gate.
//
// Deliberately absent from the UI — no input box, no toggle, no menu.
// The whole point is that operators don't know the mechanism exists, so
// they can't tell colleagues "just paste this string into the app". The
// only way to grant an install this capability is to place a specific
// file next to the exe with a specific super-password inside it.
//
// Design:
//   - being-controlled side (agent) is always available — anyone can
//     receive a connection
//   - controlling side (viewer) requires the file — clicking Connect on
//     a permission-less install pops a "缺少权限" QMessageBox
//   - permission is cached at first check; a stopped/started instance
//     re-reads on next launch, which is acceptable because the file lives
//     next to the exe and requires disk write to change
namespace permission {

// True if we're allowed to remote-control other machines.
// Missing file, wrong contents, or unreadable file all read as false.
bool HasControlPermission();

} // namespace permission
