#pragma once

#include <QString>
#include <QStringList>

// Per-machine identity that persists across launches. Stored at
// %APPDATA%\remote_desktop\local.json.
//
// On first run we synthesise an 8-digit deviceId — still short enough to
// dictate across a meeting room, and the 90M-ID pool makes birthday-style
// collisions vanishingly unlikely even with tens of thousands of installs.
// (The previous 5-digit format had a ~1% collision risk once the deployment
// reached ~130 machines — the operator kept hitting duplicate IDs.)
// There's no per-target verifyCode for this build:
// the deployment is internal-use only and the relay still enforces the
// global session_token, so an extra per-target code wouldn't add anything
// the operators wouldn't immediately memorise as a constant.
//
// recentRemoteIds is a small MRU list of devices this machine has connected
// TO, surfaced in the GUI as one-click reconnect buttons.
struct LocalConfig {
    QString     deviceId;
    QStringList recentRemoteIds;

    // KMBox NET hardware input backend. When kmboxEnabled, incoming viewer
    // key/mouse frames are routed through the physical HID box instead of
    // Windows SendInput — bypasses WeChat Work's remote-control detection.
    // Empty ip / zero port / empty uuid are the "not configured" sentinel.
    QString     kmboxIp;
    quint16     kmboxPort = 0;
    QString     kmboxUuid;
    bool        kmboxEnabled = false;
};

LocalConfig LoadOrCreateLocalConfig();

// Persists the current state. Call after mutating recentRemoteIds. Safe to
// call from the GUI thread (small file, sync write).
void SaveLocalConfig(const LocalConfig& cfg);
