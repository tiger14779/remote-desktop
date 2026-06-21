#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class RelayClient;

// Always-on background "I can be controlled" side of the app. Owns its own
// thread, connects to the relay as an Agent device, and on SESSION_START
// fires up the capture + encode + send pipeline that the standalone
// agent.exe already implements.
//
// All operator-visible state changes (registered, session up, dropped,
// reconnecting) are pushed back to the GUI via the `status` signal so the
// main window's status label stays current.
class AgentRunner : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString relayHost;
        int relayPort = 7100;
        QString deviceId;
        QString deviceToken;
        int fps = 30;
        int bitrateKbps = 8000;
        int maxHeight = 1440;
    };

    explicit AgentRunner(QObject* parent = nullptr);
    ~AgentRunner() override;

    void start(const Config& cfg);
    void stop();

signals:
    void status(const QString& text);

private:
    void runLoop(Config cfg);  // worker-thread body

    QThread workerThread_;
    std::atomic<bool> shouldExit_{false};
    Config cfg_;

    // Pointer to the currently-active relay client (lives on the stack of
    // an in-flight runLoop iteration). stop() closes it from the GUI thread
    // to wake the worker out of its blocking ReadFrame. Mutex covers both
    // the pointer swap inside runLoop and the close call from outside.
    std::mutex clientMu_;
    RelayClient* activeClient_ = nullptr;

    // Interruptible backoff sleep — stop() notifies this so retry-in-Ns
    // doesn't keep the worker alive for tens of seconds while the GUI is
    // waiting to close.
    std::mutex wakeMu_;
    std::condition_variable wakeCv_;
};
