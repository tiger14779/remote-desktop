#pragma once

#include "decode/h264_decoder.h"
#include "net/relay_client.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>

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

public slots:
    void start(Config cfg);

signals:
    void status(const QString& s);
    void frameReady(const QImage& img);
    void disconnected(const QString& reason);

private:
    ViewerRelayClient client_;
    H264Decoder decoder_;
    std::atomic<bool> stopping_{false};
};

Q_DECLARE_METATYPE(ViewerSession::Config)
