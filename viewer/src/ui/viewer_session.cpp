#include "ui/viewer_session.h"

#include "net/frame.h"
#include "udp/lan_ips.h"
#include "udp/video_xport.h"

#include <QCursor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
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
    auto statStart = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    auto statFrames = std::make_shared<int>(0);

    // Single video-bytes processor; called from BOTH the TCP onVideoFrame
    // path (legacy / before UDP_INFO arrives) and the VideoUdpXport recv
    // thread once Phase 1 UDP is up. Bytes content is identical either way
    // — a complete H.264 Annex-B frame ready for the decoder.
    auto processVideoBytes = [this, statStart, statFrames]
                              (const std::uint8_t* data, std::size_t size) {
        if (stopping_) return;
        ++(*statFrames);
        const auto now = std::chrono::steady_clock::now();
        if (now - *statStart >= std::chrono::seconds(2)) {
            const auto sinceMs = std::chrono::duration_cast<std::chrono::seconds>(
                now - *statStart).count();
            const int dropped = droppedFrames_.exchange(0);
            std::cerr << "[viewer] " << *statFrames << " frames recv in "
                      << sinceMs << "s, " << dropped << " dropped by paint backpressure\n";
            *statFrames = 0;
            *statStart = now;
        }
        std::vector<H264Decoder::DecodedFrame> frames;
        std::string err;
        const auto tDec0 = std::chrono::steady_clock::now();
        if (!decoder_.DecodePacket(data, size, &frames, &err)) {
            // Keep going on transient decode errors — a missing keyframe will
            // recover on the next IDR.
            qWarning("decode: %s", err.c_str());
            return;
        }
        const auto tDec1 = std::chrono::steady_clock::now();
        const auto decMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(tDec1 - tDec0).count();
        if (decMs > 20) {
            std::cerr << "[viewer] slow decode: " << decMs << "ms ("
                      << size << " bytes -> " << frames.size() << " frames)\n";
        }
        if (frames.empty()) return;

        // Drop everything but the latest frame in this batch — older ones
        // are already stale by the time GUI gets around to painting.
        auto& f = frames.back();

        using BufRef = std::shared_ptr<std::vector<std::uint8_t>>;
        auto* keepAlive = new BufRef(f.pixels);
        QImage img(f.pixels->data(), f.width, f.height, f.rowBytes,
                   QImage::Format_RGB32,
                   [](void* p) { delete static_cast<BufRef*>(p); },
                   keepAlive);

        // GUI backpressure WITH STASH: if a paint is in flight, overwrite
        // the stash with this frame and return. After paint completes,
        // onFramePainted will emit the stashed (latest) frame — so we
        // always paint the freshest available, never an older one.
        if (framePending_.exchange(true, std::memory_order_acq_rel)) {
            {
                std::lock_guard<std::mutex> g(stashMu_);
                stashedImg_ = img;
                hasStash_ = true;
            }
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        emit frameReady(img);
    };
    cb.onVideoFrame = processVideoBytes;
    cb.onClipboard = [this](const std::uint8_t* d, std::size_t s) {
        QString text = QString::fromUtf8(reinterpret_cast<const char*>(d),
                                          static_cast<int>(s));
        emit clipboardReceived(text);
    };
    cb.onFileXfer = [this](std::uint8_t type,
                            const std::uint8_t* d, std::size_t s) {
        emit fileXferFrameReceived(
            type,
            QByteArray(reinterpret_cast<const char*>(d), static_cast<int>(s)));
    };
    cb.onCursorShape = [this](int w, int h, int hx, int hy,
                              const std::uint8_t* bgra, std::size_t bgraSize) {
        if (w <= 0 || h <= 0 ||
            bgraSize != static_cast<std::size_t>(w) * h * 4) {
            return;  // payload doesn't match advertised dims, drop
        }
        // QImage takes a deep copy via .copy() so the wire buffer can go
        // away after this lambda returns.
        QImage img(bgra, w, h, w * 4, QImage::Format_ARGB32);
        QCursor cursor(QPixmap::fromImage(img.copy()), hx, hy);
        emit cursorShapeReceived(cursor);
    };
    cb.onDisconnect = [this]() {};

    // UDP video plane: created on demand when UDP_INFO arrives. Lifetime
    // brackets the relay client's Run() call so teardown is automatic on
    // disconnect / reconnect.
    std::shared_ptr<VideoUdpXport> udpXport;
    cb.onUdpInfo = [&udpXport, processVideoBytes, &cfg, this]
                   (const std::uint8_t* d, std::size_t s) {
        auto doc = QJsonDocument::fromJson(QByteArray::fromRawData(
            reinterpret_cast<const char*>(d), static_cast<int>(s)));
        if (!doc.isObject()) return;
        const auto obj = doc.object();
        const std::uint32_t sid = static_cast<std::uint32_t>(
            obj.value("session_id").toDouble());
        QString host = obj.value("host").toString();
        const int port = obj.value("port").toInt();
        if (host.isEmpty()) host = cfg.host;
        if (sid == 0 || port <= 0) return;
        auto x = std::make_shared<VideoUdpXport>();
        std::string e;
        if (!x->Init(host.toStdString(), port, sid,
                      VideoUdpXport::Role::Viewer, &e)) {
            std::cerr << "[viewer] UDP xport init failed: " << e << "\n";
            return;
        }
        x->SetDirectUpCallback([](const std::string& h, int p) {
            std::cerr << "[viewer] UDP direct path up — peer " << h
                      << ":" << p << " (bypassing relay)\n";
        });
        x->StartReceiver(processVideoBytes);
        udpXport = x;
        // Two spots so operators can find it: the small viewer status bar,
        // AND the app-wide log panel (via stderr -> LogSink) so the main
        // window shows it even when the viewer popup is minimised.
        emit status(QStringLiteral("UDP video plane up (relay %1:%2)")
                        .arg(host).arg(port));
        std::cerr << "[viewer] UDP video plane up (session_id=" << sid
                  << " relay=" << host.toStdString() << ":" << port << ")\n";

        // Send our LAN candidates to the peer over the relay's TCP forward.
        // Same-LAN case: the peer will probe our 192.168.x.y and get an
        // instant reply, punching direct with < 1ms latency.
        const int localUdpPort = udpXport->LocalPort();
        const auto lanIps = udp_lan::LocalIPv4Addresses();
        QJsonArray arr;
        for (const auto& ip : lanIps) {
            QJsonObject o;
            o["host"] = QString::fromStdString(ip);
            o["port"] = localUdpPort;
            arr.append(o);
        }
        QJsonObject root;
        root["candidates"] = arr;
        const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
        std::string sendErr;
        client_.SendFrameRaw(proto::MsgUdpMyCandidates,
                              reinterpret_cast<const std::uint8_t*>(json.constData()),
                              static_cast<std::size_t>(json.size()), &sendErr);
        std::cerr << "[viewer] advertised " << lanIps.size()
                  << " LAN candidate(s) via TCP\n";
    };
    // Shared parser for both flavours of candidate lists.
    auto addCandidates = [&udpXport, this](const std::uint8_t* d, std::size_t s,
                                             const char* origin) {
        if (!udpXport) return;
        auto doc = QJsonDocument::fromJson(QByteArray::fromRawData(
            reinterpret_cast<const char*>(d), static_cast<int>(s)));
        if (!doc.isObject()) return;
        const auto arr = doc.object().value("candidates").toArray();
        for (const auto& v : arr) {
            const auto obj = v.toObject();
            const QString host = obj.value("host").toString();
            const int port = obj.value("port").toInt();
            if (host.isEmpty() || port <= 0) continue;
            udpXport->AddDirectCandidate(host.toStdString(), port);
            std::cerr << "[viewer] NAT-T probing " << host.toStdString()
                      << ":" << port << " (" << origin << ")\n";
        }
    };
    cb.onUdpCandidates = [addCandidates](const std::uint8_t* d, std::size_t s) {
        addCandidates(d, s, "reflex");
    };
    cb.onUdpPeerLan = [addCandidates](const std::uint8_t* d, std::size_t s) {
        addCandidates(d, s, "lan");
    };

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

        // Tear down the UDP transport (if it ever came up) before sleeping
        // for the reconnect — it owns a recv thread that must join before
        // we redo the dance.
        if (udpXport) {
            udpXport->Stop();
            udpXport.reset();
        }

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

void ViewerSession::onFramePainted() {
    // If a fresher frame was stashed while we were painting, emit it now
    // (still under framePending_ = true so subsequent arrivals re-stash
    // instead of racing the paint). Otherwise release the gate.
    QImage next;
    bool hadStash = false;
    {
        std::lock_guard<std::mutex> g(stashMu_);
        if (hasStash_) {
            next = std::move(stashedImg_);
            stashedImg_ = QImage();
            hasStash_ = false;
            hadStash = true;
        }
    }
    if (hadStash) {
        emit frameReady(next);
    } else {
        framePending_.store(false, std::memory_order_release);
    }
}

void ViewerSession::sendKey(std::uint16_t vk, bool down) {
    std::uint8_t pl[4] = {
        static_cast<std::uint8_t>((vk >> 8) & 0xFF), static_cast<std::uint8_t>(vk & 0xFF),
        static_cast<std::uint8_t>(down ? 1 : 0), 0,
    };
    std::string err;
    client_.SendInput(proto::MsgKeyEvent, pl, sizeof(pl), &err);
}

void ViewerSession::sendClipboardText(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    std::string err;
    client_.SendClipboard(reinterpret_cast<const std::uint8_t*>(utf8.constData()),
                          static_cast<std::size_t>(utf8.size()), &err);
}

void ViewerSession::sendFrameRaw(std::uint8_t type, const QByteArray& payload) {
    std::string err;
    client_.SendFrameRaw(
        type,
        reinterpret_cast<const std::uint8_t*>(payload.constData()),
        static_cast<std::size_t>(payload.size()), &err);
}
