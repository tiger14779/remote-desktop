#pragma once

#include "decode/h264_decoder.h"
#include "net/relay_client.h"

#include <QCursor>
#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <mutex>

// Worker QObject that lives on a background QThread. Receives VIDEO_FRAME
// from the relay, decodes, converts to QImage, and emits frameReady to the GUI
// thread via a queued connection.
class ViewerSession : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString host;
        int port;
        QString account;
        QString sessionToken;
        QString deviceId;
    };

    explicit ViewerSession(QObject* parent = nullptr);

    // Closes the underlying socket. Safe to call from the GUI thread; it will
    // unblock Run() on the worker thread.
    void requestStop();

    // Input send methods — callable from any thread. They delegate to
    // ViewerRelayClient::SendInput which is mutex-serialised internally, so
    // we deliberately do NOT cross-thread through the event loop (the worker
    // thread is busy inside Run() and would never service queued slots).
    void sendMouseMove(std::uint16_t x, std::uint16_t y);
    void sendMouseButton(std::uint8_t button, bool down);
    void sendKey(std::uint16_t vkCode, bool down);

    // Pushes the viewer-side clipboard text to the agent. No-op if the
    // session is not active.
    void sendClipboardText(const QString& text);

    // Generic frame writer (used by ClipboardFileXfer). Safe to call from
    // any thread — wraps ViewerRelayClient::SendFrameRaw which holds the
    // write mutex internally.
    void sendFrameRaw(std::uint8_t type, const QByteArray& payload);

    // Called on the GUI thread by VideoWidget after paintEvent finishes.
    // Just clears the framePending_ flag so the next decoded frame is
    // allowed through. Direct-connected so it's a cheap atomic flip.
    void onFramePainted();

public slots:
    void start(Config cfg);

signals:
    void status(const QString& s);
    void frameReady(const QImage& img);
    void disconnected(const QString& reason);
    // Fired on the GUI thread when the agent pushes its clipboard contents
    // into this viewer. Subscribers should set the local clipboard.
    void clipboardReceived(const QString& text);
    // Fired on the GUI thread when the agent reports a new cursor shape.
    // Subscribers should set the new shape on the video widget.
    void cursorShapeReceived(const QCursor& cursor);
    // FILE_XFER_BEGIN/CHUNK/END from the agent. Payload is the raw frame
    // body — ClipboardFileXfer parses it.
    void fileXferFrameReceived(std::uint8_t type, const QByteArray& payload);

private:
    // Sleeps for up to dt, but returns early if requestStop() fires.
    // Returns true if stop was requested during the wait.
    bool waitOrStop(std::chrono::milliseconds dt);

    ViewerRelayClient client_;
    H264Decoder decoder_;
    std::atomic<bool> stopping_{false};

    // Used by requestStop() to wake the reconnect-backoff sleep.
    std::mutex stopMu_;
    std::condition_variable stopCv_;

    // GUI backpressure. Network thread sets true before emit, GUI thread
    // clears it in onFramePainted. If true when a new frame is decoded,
    // we STASH it (overwriting any previous stash) and emit it after the
    // in-flight paint finishes. The stash is critical: at the end of a
    // fast drag, the LAST frame arrives while we're still painting an
    // intermediate one — if we just dropped it like the old code did, the
    // viewer would forever show the intermediate state and never the
    // window's final position (the "stuck mid-drag" symptom).
    std::atomic<bool> framePending_{false};
    std::mutex stashMu_;
    QImage stashedImg_;
    bool hasStash_ = false;

    // Diagnostic: how many frames the paint backpressure absorbed (replaced
    // by a fresher one in the stash) since the last log.
    std::atomic<int> droppedFrames_{0};

public:
    // Paint timing sinks, populated by VideoWidget on the GUI thread every
    // paintEvent. Read + reset in the per-2s stats print. Public because
    // VideoWidget is a plain widget lambda-forwarding paint events at us;
    // no need for a whole friend declaration.
    std::atomic<int>       paintMaxMs_{0};
    std::atomic<long long> paintSumMs_{0};
    std::atomic<int>       paintCount_{0};
};

Q_DECLARE_METATYPE(ViewerSession::Config)
