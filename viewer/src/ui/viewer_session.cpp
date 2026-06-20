#include "ui/viewer_session.h"

#include "net/frame.h"

#include <QImage>

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

ViewerSession::ViewerSession(QObject* parent) : QObject(parent) {}

void ViewerSession::requestStop() {
    {
        std::lock_guard<std::mutex> g(stopMu_);
        stopping_ = true;
    }
    stopCv_.notify_all();
    client_.Close();
}

bool ViewerSession::waitOrStop(std::chrono::milliseconds dt) {
    std::unique_lock<std::mutex> lock(stopMu_);
    stopCv_.wait_for(lock, dt, [this]() { return stopping_.load(); });
    return stopping_.load();
}

void ViewerSession::start(Config cfg) {
    std::string err;
    std::string decoderMode;
    if (!decoder_.Init(&decoderMode, &err)) {
        emit disconnected(QStringLiteral("decoder init: ") + QString::fromStdString(err));
        return;
    }
    emit status(QStringLiteral("decoder: %1").arg(QString::fromStdString(decoderMode)));

    ViewerRelayClient::Config rc;
    rc.host = cfg.host.toStdString();
    rc.port = cfg.port;
    rc.account = cfg.account.toStdString();
    rc.sessionToken = cfg.sessionToken.toStdString();
    rc.deviceId = cfg.deviceId.toStdString();

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
            // Wrap the decoder's shared buffer in a QImage with a cleanup
            // callback so QImage's implicit refcount drives the buffer's
            // lifetime — no memcpy on the network -> GUI thread hop.
            using BufRef = std::shared_ptr<std::vector<std::uint8_t>>;
            auto* keepAlive = new BufRef(f.pixels);
            QImage img(f.pixels->data(), f.width, f.height, f.rowBytes,
                       QImage::Format_RGB32,
                       [](void* p) { delete static_cast<BufRef*>(p); },
                       keepAlive);
            emit frameReady(img);
        }
    };
    cb.onDisconnect = [this]() {};

    // Reconnect loop with exponential backoff: 1, 2, 4, 8, 16, 30, 30...
    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    while (!stopping_) {
        client_.Close();  // idempotent; safe if no active socket
        emit status(QStringLiteral("connecting %1:%2 ...").arg(cfg.host).arg(cfg.port));
        if (!client_.ConnectAuthorize(rc, &err)) {
            if (stopping_) break;
            emit status(QStringLiteral("connect failed: %1 — retry in %2s")
                            .arg(QString::fromStdString(err)).arg(backoffSec));
            if (waitOrStop(std::chrono::seconds(backoffSec))) break;
            backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
            continue;
        }
        emit status(QStringLiteral("connected (%1), waiting for frames")
                        .arg(QString::fromStdString(decoderMode)));
        backoffSec = 1;

        // Heartbeat thread. 5s cadence, wakes itself every 100ms so we shut
        // down quickly on disconnect / stop.
        std::atomic<bool> hbStop{false};
        std::thread hb([this, &hbStop]() {
            while (!hbStop.load()) {
                for (int i = 0; i < 50 && !hbStop.load(); ++i) {
                    std::this_thread::sleep_for(100ms);
                }
                if (hbStop.load()) break;
                std::string hbErr;
                if (!client_.SendHeartbeat(&hbErr)) break;
            }
        });

        client_.Run(cb);

        hbStop.store(true);
        hb.join();

        if (stopping_) break;
        emit status(QStringLiteral("disconnected — reconnecting in %1s").arg(backoffSec));
        if (waitOrStop(std::chrono::seconds(backoffSec))) break;
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }

    client_.Close();
    emit disconnected(stopping_ ? QStringLiteral("closed by user")
                                : QStringLiteral("session ended"));
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
