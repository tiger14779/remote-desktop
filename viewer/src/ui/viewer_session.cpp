#include "ui/viewer_session.h"

#include "net/frame.h"

#include <QImage>

#include <string>

ViewerSession::ViewerSession(QObject* parent) : QObject(parent) {}

void ViewerSession::requestStop() {
    stopping_ = true;
    client_.Close();
}

void ViewerSession::start(Config cfg) {
    std::string err;
    if (!decoder_.Init(&err)) {
        emit disconnected(QStringLiteral("decoder init: ") + QString::fromStdString(err));
        return;
    }

    ViewerRelayClient::Config rc;
    rc.host = cfg.host.toStdString();
    rc.port = cfg.port;
    rc.account = cfg.account.toStdString();
    rc.sessionToken = cfg.sessionToken.toStdString();
    rc.deviceId = cfg.deviceId.toStdString();

    emit status(QStringLiteral("authenticating..."));
    if (!client_.ConnectAuthorize(rc, &err)) {
        emit disconnected(QString::fromStdString(err));
        return;
    }
    emit status(QStringLiteral("connected, waiting for frames"));

    ViewerRelayClient::Callbacks cb;
    cb.onVideoFrame = [this](const std::uint8_t* data, std::size_t size) {
        if (stopping_) return;
        std::vector<H264Decoder::DecodedFrame> frames;
        std::string err;
        if (!decoder_.DecodePacket(data, size, &frames, &err)) {
            // Keep going on transient decode errors — a missing keyframe will
            // recover on the next IDR.
            qWarning("decode: %s", err.c_str());
            return;
        }
        for (auto& f : frames) {
            // Wrap the buffer in a QImage and deep-copy so the receiver can
            // hold onto it after the DecodedFrame goes out of scope.
            QImage shallow(f.pixels.data(), f.width, f.height, f.rowBytes,
                           QImage::Format_RGB32);
            emit frameReady(shallow.copy());
        }
    };
    cb.onDisconnect = [this]() {};

    client_.Run(cb);

    if (stopping_) {
        emit disconnected(QStringLiteral("closed by user"));
    } else {
        emit disconnected(QStringLiteral("connection lost"));
    }
}

void ViewerSession::sendMouseMove(std::uint16_t x, std::uint16_t y) {
    std::uint8_t pl[4] = {
        static_cast<std::uint8_t>((x >> 8) & 0xFF), static_cast<std::uint8_t>(x & 0xFF),
        static_cast<std::uint8_t>((y >> 8) & 0xFF), static_cast<std::uint8_t>(y & 0xFF),
    };
    std::string err;
    client_.SendInput(proto::MsgMouseMove, pl, sizeof(pl), &err);
    // Errors here are usually "socket closed" — onDisconnected will handle it.
}

void ViewerSession::sendMouseButton(std::uint8_t button, bool down) {
    std::uint8_t pl[2] = { button, static_cast<std::uint8_t>(down ? 1 : 0) };
    std::string err;
    client_.SendInput(proto::MsgMouseButton, pl, sizeof(pl), &err);
}

void ViewerSession::sendKey(std::uint16_t vk, bool down) {
    std::uint8_t pl[4] = {
        static_cast<std::uint8_t>((vk >> 8) & 0xFF), static_cast<std::uint8_t>(vk & 0xFF),
        static_cast<std::uint8_t>(down ? 1 : 0), 0,
    };
    std::string err;
    client_.SendInput(proto::MsgKeyEvent, pl, sizeof(pl), &err);
}
