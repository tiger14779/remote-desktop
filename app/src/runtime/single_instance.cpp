#include "runtime/single_instance.h"

#include <QLocalServer>
#include <QLocalSocket>

#ifdef _WIN32
#include <windows.h>
#endif

SingleInstance::SingleInstance(const QString& key, QObject* parent)
    : QObject(parent), key_(key) {}

SingleInstance::~SingleInstance() {
    if (server_) {
        server_->close();
        // server_ is parented to us — deletion happens on QObject teardown.
    }
}

bool SingleInstance::tryStartAsPrimary() {
    // Fast-path: probe whether an existing server is listening on our name.
    // If connect succeeds within a short timeout, an instance is up and we
    // are secondary. Longer probe timeouts would make cold starts feel
    // laggy for no benefit — the listen-or-not decision is immediate.
    {
        QLocalSocket probe;
        probe.connectToServer(key_);
        if (probe.waitForConnected(200)) {
            probe.disconnectFromServer();
            return false;
        }
    }
    // A crashed previous primary can leave the socket file dangling; the
    // OS won't clean it up on Linux/macOS, and on Windows the named pipe
    // is auto-removed but the "server address in use" bookkeeping can
    // still confuse listen(). removeServer() clears both.
    QLocalServer::removeServer(key_);
    server_ = new QLocalServer(this);
    // Restrict the socket to the current user so a different user on the
    // same machine (fast-user-switch / RDP secondary session) doesn't
    // trample our IPC endpoint.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(server_, &QLocalServer::newConnection,
                     this, &SingleInstance::onNewConnection);
    if (!server_->listen(key_)) {
        // Fall through as "we are primary" rather than fail the launch:
        // the alternative is refusing to start at all, which is worse than
        // running without the single-instance guarantee.
        return true;
    }
    return true;
}

bool SingleInstance::notifyRunningInstance() {
#ifdef _WIN32
    // Give the primary permission to steal foreground from us. On Windows
    // the foreground-window lock rejects SetForegroundWindow calls from
    // processes that aren't currently foreground, unless the target
    // process has been explicitly whitelisted by the current foreground
    // process — that's what ASFW_ANY (-1) does. Without this, launching
    // the app while its main window is hidden to tray results in the tray
    // icon lighting up but nothing actually appearing on screen.
    ::AllowSetForegroundWindow(ASFW_ANY);
#endif
    QLocalSocket s;
    s.connectToServer(key_);
    if (!s.waitForConnected(500)) return false;
    // Payload isn't parsed today, but writing a newline-terminated token
    // keeps the wire format usable for future commands (e.g. "connect
    // <id>" so a URL-scheme invocation can pass the target).
    s.write("show\n");
    s.flush();
    s.waitForBytesWritten(500);
    s.disconnectFromServer();
    return true;
}

void SingleInstance::onNewConnection() {
    while (auto* c = server_->nextPendingConnection()) {
        QObject::connect(c, &QLocalSocket::disconnected,
                         c, &QLocalSocket::deleteLater);
        // Drain incoming so the peer's flush doesn't stall. We don't parse
        // the token — the mere connection is the wake signal today.
        QObject::connect(c, &QLocalSocket::readyRead, c, [c]() {
            (void)c->readAll();
        });
        emit wakeupRequested();
    }
}
