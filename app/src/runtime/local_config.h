#pragma once

#include <QString>

// Per-machine identity that persists across launches. Stored at
// %APPDATA%\remote_desktop\local.json so the same physical machine keeps
// the same ID/code without anyone touching a config file by hand.
//
// On first run we synthesise:
//   - deviceId   : 9-digit random number, RustDesk-style
//   - verifyCode : 8-char alphanumeric, what someone else must type to
//                  connect TO this machine
//
// For the MVP the relay still authenticates via the fixed admin /
// session_token pair from config.production.json — the verifyCode is
// displayed for the operator's mental model but isn't yet enforced on the
// relay side. T2-7 onwards: extend CONNECT to carry the target's
// verifyCode and have the agent compare it.
struct LocalConfig {
    QString deviceId;
    QString verifyCode;
};

LocalConfig LoadOrCreateLocalConfig();
