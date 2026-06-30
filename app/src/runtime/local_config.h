#pragma once

#include <QString>
#include <QStringList>

// Per-machine identity that persists across launches. Stored at
// %APPDATA%\remote_desktop\local.json.
//
// On first run we synthesise a 5-digit deviceId — short enough to dictate
// across a meeting room, large enough to give 90K IDs in the dynamic
// registration pool. There's no per-target verifyCode for this build:
// the deployment is internal-use only and the relay still enforces the
// global session_token, so an extra per-target code wouldn't add anything
// the operators wouldn't immediately memorise as a constant.
//
// recentRemoteIds is a small MRU list of devices this machine has connected
// TO, surfaced in the GUI as one-click reconnect buttons.
struct LocalConfig {
    QString     deviceId;
    QStringList recentRemoteIds;
};

LocalConfig LoadOrCreateLocalConfig();

// Persists the current state. Call after mutating recentRemoteIds. Safe to
// call from the GUI thread (small file, sync write).
void SaveLocalConfig(const LocalConfig& cfg);
