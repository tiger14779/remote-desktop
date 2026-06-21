#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

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
};
