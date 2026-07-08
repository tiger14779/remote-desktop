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

    // Sends UTF-8 clipboard text to the connected viewer (no-op if no viewer
    // is currently attached). Safe to call from the GUI thread.
    void sendClipboardText(const QString& text);

    // Generic frame writer (used by ClipboardFileXfer for the FILE_XFER_*
    // family). No-op if no viewer attached. Safe to call from GUI thread.
    void sendFrameRaw(std::uint8_t type, const QByteArray& payload);

    // Diagnostic switch: when true, incoming input events from the viewer
    // are received and logged but NOT actually injected into Windows via
    // SendInput. Purpose: prove whether WeChat Work's "remote control"
    // alert is triggered by SendInput or by our screen-capture path.
    // Safe to toggle from the GUI thread; read from the socket thread.
    void setSkipInput(bool skip) { skipInput_.store(skip); }
    bool skipInput() const { return skipInput_.load(); }

signals:
    void status(const QString& text);
    // Fired on the GUI thread when the viewer pushes its clipboard contents
    // into this agent. Subscribers should set the local clipboard.
    void clipboardReceived(const QString& text);
    // FILE_XFER_BEGIN/CHUNK/END from the viewer. Payload is the raw frame
    // body — the bridge parses it.
    void fileXferFrameReceived(std::uint8_t type, const QByteArray& payload);

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

    // See setSkipInput(): diagnostic flag flipped from the GUI.
    std::atomic<bool> skipInput_{false};
};
