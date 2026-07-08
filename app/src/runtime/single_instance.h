#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

// Per-user "only one running at a time" gate for the remote-desktop app,
// built on QLocalServer / QLocalSocket.
//
// Why not a CreateMutex approach? CreateMutex tells you IF another instance
// is running but not HOW to reach it — we still need an IPC channel to say
// "please raise your window". QLocalSocket gives us both: connect() fails
// with error "not found" if no primary is up (we then become primary), and
// succeeds otherwise, at which point we push a wake byte over it.
//
// The key is scoped to the current user (UserAccessOption) so two different
// users on the same terminal server each get their own primary — different
// desktop sessions with different device IDs.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString& key, QObject* parent = nullptr);
    ~SingleInstance() override;

    // Try to claim primary status. Returns true if we succeed (nothing else
    // was listening). Returns false if another instance is already up.
    //
    // On the false return, call notifyRunningInstance() to ask that instance
    // to bring itself forward, then exit this process — the user just
    // clicked "launch again" and expects the running window to pop up.
    bool tryStartAsPrimary();

    // Best-effort "poke the primary" from a secondary process. Also grants
    // the primary permission to steal the foreground focus from us on
    // Windows (ASFW_ANY) — otherwise showing a hidden-to-tray window from
    // a background process silently fails and the user sees nothing.
    // Returns true if the wake byte was written.
    bool notifyRunningInstance();

signals:
    // Fired on the primary's GUI thread whenever a secondary connects.
    // Handler should raise the main window to the foreground.
    void wakeupRequested();

private:
    void onNewConnection();

    QString       key_;
    QLocalServer* server_ = nullptr;
};
