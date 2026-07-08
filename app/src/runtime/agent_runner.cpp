#include "runtime/agent_runner.h"

#include "capture/dxgi_capturer.h"
#include "encode/h264_encoder.h"
#include "input/input_injector.h"
#include "net/frame.h"
#include "net/agent_relay_client.h"
#include "udp/lan_ips.h"
#include "udp/video_xport.h"

#include <d3d11.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Capture+encode pump. Spends most of its life waiting on `sessionActive`,
// fires up when a viewer connects.
class StreamPump {
public:
    StreamPump(DxgiCapturer* cap, RelayClient* client,
               int fps, int bitrateKbps, int maxHeight)
        : cap_(cap), client_(client),
          fps_(fps), bitrateKbps_(bitrateKbps), maxHeight_(maxHeight) {}

    void Start() { thread_ = std::thread([this]() { run(); }); }

    // Swap the video send path to a UDP transport (Phase 1 P2P/UDP-relay
    // upgrade). Null = fall back to the old TCP MsgVideoFrame path. Safe to
    // call from any thread; the pump picks up the change before its next
    // encode-and-send.
    void SetUdpXport(VideoUdpXport* udp) {
        std::lock_guard<std::mutex> g(xportMu_);
        udp_ = udp;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            shouldExit_ = true;
            sessionActive_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void OnSessionStart() {
        { std::lock_guard<std::mutex> g(mu_); sessionActive_ = true; }
        cv_.notify_all();
    }
    void OnSessionEnd() {
        { std::lock_guard<std::mutex> g(mu_); sessionActive_ = false; }
        cv_.notify_all();
    }

private:
    void run() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]() { return sessionActive_ || shouldExit_; });
                if (shouldExit_) return;
            }
            streamUntilSessionEnd();
        }
    }

    void streamUntilSessionEnd() {
        DxgiCapturer::Frame frame;
        std::string err;
        while (sessionActive_) {
            auto r = cap_->CaptureFrame(100, &frame, &err);
            if (r == DxgiCapturer::Result::kFrame) break;
            if (r == DxgiCapturer::Result::kError) {
                std::cerr << "[agent] capture error: " << err << " — reinit\n";
                if (!cap_->Init(&err)) std::this_thread::sleep_for(500ms);
            }
        }
        if (!sessionActive_) return;

        H264Encoder enc;
        H264Encoder::Config ec;
        ec.srcWidth = frame.width;
        ec.srcHeight = frame.height;
        if (maxHeight_ > 0 && frame.height > maxHeight_) {
            ec.height = maxHeight_;
            ec.width = static_cast<int>(
                static_cast<int64_t>(frame.width) * maxHeight_ / frame.height);
            ec.width &= ~1;
            ec.height &= ~1;
        } else {
            ec.width = frame.width & ~1;
            ec.height = frame.height & ~1;
        }
        ec.fps = fps_;
        ec.bitrateKbps = bitrateKbps_;
        ec.gopSize = fps_;
        // Zero-copy path is only sound when source and encoder dimensions
        // match — NVENC won't scale a D3D11 input texture for us. If we're
        // asking for a downscale (maxHeight cap kicked in), fall back to
        // the CPU sws_scale path so the resize actually happens.
        const bool sizeMatches =
            ec.width == ec.srcWidth && ec.height == ec.srcHeight;
        if (sizeMatches) {
            ec.d3d11Device = cap_->D3D11Device();
        }
        std::string chosen;
        if (!enc.Init(ec, &chosen, &err)) {
            std::cerr << "[agent] encoder init failed: " << err << "\n";
            return;
        }
        std::cerr << "[agent] session: " << ec.srcWidth << "x" << ec.srcHeight
                  << " -> " << ec.width << "x" << ec.height
                  << " @ " << ec.fps << "fps (" << chosen
                  << (enc.IsGpuInput() ? ", GPU direct" : ", CPU BGRA")
                  << ")\n";

        // Used as the AcquireNextFrame timeout: block up to one frame-period
        // waiting for a new present, then move on so a long idle still gets
        // statistics + the sessionActive_ check.
        const int captureTimeoutMs =
            std::max(1, static_cast<int>(std::chrono::milliseconds(1000 / fps_).count()));
        int64_t pts = 0;
        // First-frame send happens on the first main-loop iteration below —
        // for GPU mode this must go through EncodeFrameGpu, so we can't
        // just call the CPU encodeAndSend eagerly here.
        // No idle-triggered IDR. intra-refresh is on, so every gop_size frames
        // the whole picture is already being rolled through I-stripes — the
        // decoder is always self-recovering and never needs a "fresh start"
        // IDR after a quiet period. The earlier wasIdle path was firing a
        // 600KB IDR after every click in a slowly-clicked file list, which
        // dominated perceived latency on small UI changes.

        // Per-frame + per-2s diagnostics. encode_ms and send_ms are reported
        // separately so the operator can tell "encoder is slow on this
        // machine" from "viewer is reading slow and TCP back-pressured my
        // send" — they look identical with a combined timer.
        auto statStart = std::chrono::steady_clock::now();
        int statFrames = 0;
        std::size_t statBytes = 0;
        long long statMaxEnc = 0;
        long long statMaxSend = 0;
        std::size_t statMaxBytes = 0;

        // Heartbeat: if DXGI hasn't reported a new present in this long, the
        // user can be stuck staring at a stale frame on the viewer. DXGI's
        // "no fire when nothing changes" semantics + Windows 11 DWM's habit
        // of compositing small UI updates (file list highlight, etc.) via
        // DirectComposition that DXGI Desktop Duplication doesn't always
        // pick up mean the cached frame is sometimes "behind reality" by
        // an unbounded amount until the user nudges the desktop. We force
        // re-encode at this cadence so the pipeline stays warm and we
        // catch any DXGI fire within roughly one heartbeat-period.
        constexpr auto kHeartbeatInterval = std::chrono::milliseconds(50);
        auto lastSentAt = std::chrono::steady_clock::now();
        int statRealFrames = 0;
        int statHeartbeatFrames = 0;

        // Frame-rate cap. Before Phase A (CPU encode) the encode cost of
        // ~12ms was a natural throttle; now encode is 3ms and DXGI on
        // hi-Hz monitors happily fires 200+ times per second, blowing
        // through bitrate targets and giving the viewer a queue of
        // ~150ms of frames it can't paint fast enough. This deadline
        // paces the loop at the configured fps so the produced stream
        // actually matches its ctx->framerate metadata (rate control
        // depends on this) and the viewer never has more than one
        // frame's worth in flight.
        const auto frameInterval = std::chrono::microseconds(1000000 / fps_);
        auto frameDeadline = std::chrono::steady_clock::now();

        // GPU-mode state: cache the last-captured D3D11 texture so heartbeat
        // re-encodes work without another capture. The pointer stays valid
        // for the lifetime of DxgiCapturer's internal gpu pool.
        DxgiCapturer::GpuFrame gpuFrame;
        ID3D11Texture2D* lastGpuTex = nullptr;
        const bool gpuMode = enc.IsGpuInput();
        // Seed the GPU pool with a first capture in GPU mode so the
        // heartbeat path has something to re-send from the very start.
        if (gpuMode) {
            for (int i = 0; i < 20 && sessionActive_ && !lastGpuTex; ++i) {
                auto r = cap_->CaptureFrameGpu(100, &gpuFrame, &err);
                if (r == DxgiCapturer::Result::kFrame) {
                    lastGpuTex = gpuFrame.gpuTexture;
                }
            }
        }

        while (sessionActive_) {
            // Sleep until the next frame slot so we send at most fps_
            // frames/sec regardless of how fast encode/DXGI actually run.
            // First iter has frameDeadline == now, so it doesn't sleep.
            {
                const auto now = std::chrono::steady_clock::now();
                if (now < frameDeadline) {
                    std::this_thread::sleep_until(frameDeadline);
                }
                frameDeadline = std::chrono::steady_clock::now() + frameInterval;
            }
            // Short capture timeout so the heartbeat check still fires
            // promptly when DXGI sits silent for a long time.
            const int hbMs = static_cast<int>(kHeartbeatInterval.count());
            const int waitMs = std::min(captureTimeoutMs, hbMs);
            DxgiCapturer::Result r;
            DxgiCapturer::Frame fresh;   // CPU path scratch
            if (gpuMode) {
                r = cap_->CaptureFrameGpu(waitMs, &gpuFrame, &err);
            } else {
                r = cap_->CaptureFrame(waitMs, &fresh, &err);
            }
            if (r == DxgiCapturer::Result::kError) {
                std::cerr << "[agent] capture error: " << err << " — reinit\n";
                if (!cap_->Init(&err)) std::this_thread::sleep_for(500ms);
                continue;
            }
            bool isHeartbeat = false;
            if (r == DxgiCapturer::Result::kFrame) {
                if (gpuMode) {
                    lastGpuTex = gpuFrame.gpuTexture;
                } else {
                    frame = std::move(fresh);
                }
            } else {
                // No new DXGI frame. Only re-send if the heartbeat interval
                // has elapsed (don't flood the encoder with duplicates).
                if (std::chrono::steady_clock::now() - lastSentAt < kHeartbeatInterval) {
                    continue;
                }
                isHeartbeat = true;
            }

            std::size_t thisBytes = 0;
            long long encMs = 0, sendMs = 0;
            bool ok;
            if (gpuMode) {
                if (!lastGpuTex) continue;  // nothing to send yet
                ok = encodeAndSendGpu(enc, lastGpuTex, pts++, /*forceIdr=*/false,
                                      &thisBytes, &encMs, &sendMs, &err);
            } else {
                ok = encodeAndSend(enc, frame, pts++, /*forceIdr=*/false,
                                    &thisBytes, &encMs, &sendMs, &err);
            }
            if (!ok) {
                std::cerr << "[agent] encode/send failed: " << err << "\n";
                return;
            }
            const auto t1 = std::chrono::steady_clock::now();
            lastSentAt = t1;
            if (isHeartbeat) ++statHeartbeatFrames;
            else              ++statRealFrames;

            // Cursor shape forwarding is wired but disabled: the
            // monochrome/masked-color decode created I-beams that
            // obscured underlying text on the viewer side, and the user
            // preferred the plain Qt arrow over a half-working
            // shape-follow. Code path stays intact for a future re-enable;
            // just drain DXGI's pending shape so we don't accumulate it.
            DxgiCapturer::CursorShape shape;
            cap_->TakeCursorShape(&shape);

            // Per-frame warning. If encode_ms is the big number, libx264
            // is the bottleneck on this CPU. If send_ms is big, viewer is
            // reading slow and TCP back-pressure is what we're seeing.
            if (encMs > 50 || sendMs > 50 || thisBytes > 200 * 1024) {
                std::cerr << "[stat] heavy frame: enc=" << encMs << "ms"
                          << " send=" << sendMs << "ms"
                          << " " << (thisBytes / 1024) << "KB\n";
            }
            statFrames++;
            statBytes += thisBytes;
            if (encMs > statMaxEnc) statMaxEnc = encMs;
            if (sendMs > statMaxSend) statMaxSend = sendMs;
            if (thisBytes > statMaxBytes) statMaxBytes = thisBytes;
            const auto since =
                std::chrono::duration_cast<std::chrono::seconds>(t1 - statStart).count();
            if (since >= 2) {
                std::cerr << "[stat] " << statFrames << " frames in " << since
                          << "s ("
                          << statRealFrames << " dxgi + "
                          << statHeartbeatFrames << " hb)"
                          << ", total " << (statBytes / 1024) << "KB"
                          << ", max-enc " << statMaxEnc << "ms"
                          << ", max-send " << statMaxSend << "ms"
                          << ", max-frame " << (statMaxBytes / 1024) << "KB\n";
                statStart = t1;
                statFrames = 0;
                statBytes = 0;
                statMaxEnc = 0;
                statMaxSend = 0;
                statMaxBytes = 0;
                statRealFrames = 0;
                statHeartbeatFrames = 0;
            }
        }
    }

    // GPU-direct variant: encode straight from a D3D11 texture the caller
    // just captured with DxgiCapturer::CaptureFrameGpu. Same output shape
    // as encodeAndSend so callers can pick per frame.
    bool encodeAndSendGpu(H264Encoder& enc, ID3D11Texture2D* tex,
                          int64_t pts, bool forceIdr,
                          std::size_t* bytesOut,
                          long long* encodeMsOut,
                          long long* sendMsOut,
                          std::string* err) {
        std::vector<std::vector<std::uint8_t>> pkts;
        const auto t0 = std::chrono::steady_clock::now();
        if (!enc.EncodeFrameGpu(tex, pts, forceIdr, &pkts, err)) return false;
        const auto tEnc = std::chrono::steady_clock::now();
        VideoUdpXport* xport = nullptr;
        {
            std::lock_guard<std::mutex> g(xportMu_);
            xport = udp_;
        }
        std::size_t total = 0;
        for (const auto& p : pkts) {
            bool ok = xport ? xport->SendFrame(p.data(), p.size(), err)
                            : client_->SendVideoFrame(p.data(), p.size(), err);
            if (!ok) return false;
            total += p.size();
        }
        const auto tSent = std::chrono::steady_clock::now();
        if (encodeMsOut) *encodeMsOut =
            std::chrono::duration_cast<std::chrono::milliseconds>(tEnc - t0).count();
        if (sendMsOut) *sendMsOut =
            std::chrono::duration_cast<std::chrono::milliseconds>(tSent - tEnc).count();
        if (bytesOut) *bytesOut = total;
        return true;
    }

    bool encodeAndSend(H264Encoder& enc, const DxgiCapturer::Frame& f,
                       int64_t pts, bool forceIdr,
                       std::size_t* bytesOut,
                       long long* encodeMsOut,
                       long long* sendMsOut,
                       std::string* err) {
        std::vector<std::vector<std::uint8_t>> pkts;
        const auto t0 = std::chrono::steady_clock::now();
        if (!enc.EncodeFrame(f.pixels.data(), f.rowPitch, pts, forceIdr, &pkts, err)) {
            return false;
        }
        const auto tEnc = std::chrono::steady_clock::now();
        VideoUdpXport* xport = nullptr;
        {
            std::lock_guard<std::mutex> g(xportMu_);
            xport = udp_;
        }
        std::size_t total = 0;
        for (const auto& p : pkts) {
            bool ok;
            if (xport) {
                // UDP path: one xport->SendFrame per encoded NAL — the xport
                // internally fragments to fit UDP MTU. Failure on UDP is
                // non-fatal (datagrams are unreliable anyway), so we just
                // log via err and keep going.
                ok = xport->SendFrame(p.data(), p.size(), err);
            } else {
                ok = client_->SendVideoFrame(p.data(), p.size(), err);
            }
            if (!ok) return false;
            total += p.size();
        }
        const auto tSent = std::chrono::steady_clock::now();
        if (encodeMsOut) *encodeMsOut =
            std::chrono::duration_cast<std::chrono::milliseconds>(tEnc - t0).count();
        if (sendMsOut) *sendMsOut =
            std::chrono::duration_cast<std::chrono::milliseconds>(tSent - tEnc).count();
        if (bytesOut) *bytesOut = total;
        return true;
    }

    DxgiCapturer* cap_;
    RelayClient* client_;
    int fps_, bitrateKbps_, maxHeight_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool sessionActive_ = false;
    bool shouldExit_ = false;

    // UDP video path (Phase 1). Owned by AgentRunner::runLoop; we just hold
    // a weak pointer here. nullptr means "fall back to TCP video frames" —
    // the state the pump runs in until UDP_INFO is received.
    std::mutex xportMu_;
    VideoUdpXport* udp_ = nullptr;
};

} // namespace

AgentRunner::AgentRunner(QObject* parent) : QObject(parent) {}

AgentRunner::~AgentRunner() { stop(); }

void AgentRunner::start(const Config& cfg) {
    cfg_ = cfg;
    shouldExit_ = false;
    workerThread_.start();
    moveToThread(&workerThread_);
    QMetaObject::invokeMethod(this, [this]() { runLoop(cfg_); }, Qt::QueuedConnection);
}

void AgentRunner::sendClipboardText(const QString& text) {
    std::lock_guard<std::mutex> g(clientMu_);
    if (!activeClient_) return;  // no viewer connected, drop
    const QByteArray utf8 = text.toUtf8();
    std::string err;
    activeClient_->SendClipboard(
        reinterpret_cast<const std::uint8_t*>(utf8.constData()),
        static_cast<std::size_t>(utf8.size()), &err);
    // Errors here are usually "socket closed" — the read loop will tear down
    // the session on its own; we drop quietly.
}

void AgentRunner::sendFrameRaw(std::uint8_t type, const QByteArray& payload) {
    std::lock_guard<std::mutex> g(clientMu_);
    if (!activeClient_) return;
    std::string err;
    activeClient_->SendFrameRaw(
        type,
        reinterpret_cast<const std::uint8_t*>(payload.constData()),
        static_cast<std::size_t>(payload.size()), &err);
}

void AgentRunner::stop() {
    shouldExit_ = true;

    // Wake the worker if it's currently blocked in a backoff sleep.
    wakeCv_.notify_all();

    // If the worker is currently inside RelayClient::Run() (blocking on
    // ReadFull), it won't notice shouldExit_ on its own. Closing the socket
    // is what unblocks ReadFull and lets runLoop unwind cleanly.
    {
        std::lock_guard<std::mutex> g(clientMu_);
        if (activeClient_) activeClient_->Close();
    }

    if (workerThread_.isRunning()) {
        workerThread_.quit();
        workerThread_.wait();
    }
}

void AgentRunner::runLoop(Config cfg) {
    emit status(QStringLiteral("initialising capture..."));
    DxgiCapturer cap;
    std::string err;
    if (!cap.Init(&err)) {
        emit status(QStringLiteral("capture init failed: %1")
                        .arg(QString::fromStdString(err)));
        return;
    }

    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    auto sleepOrExit = [this](int seconds) {
        std::unique_lock<std::mutex> lock(wakeMu_);
        wakeCv_.wait_for(lock, std::chrono::seconds(seconds),
                          [this]() { return shouldExit_.load(); });
    };

    while (!shouldExit_.load()) {
        RelayClient client;
        RelayClient::Config rc;
        rc.host = cfg.relayHost.toStdString();
        rc.port = cfg.relayPort;
        rc.deviceId = cfg.deviceId.toStdString();
        rc.deviceToken = cfg.deviceToken.toStdString();

        emit status(QStringLiteral("connecting to %1:%2 ...")
                        .arg(cfg.relayHost).arg(cfg.relayPort));

        if (!client.ConnectAndRegister(rc, &err)) {
            emit status(QStringLiteral("relay: %1 — retry in %2s")
                            .arg(QString::fromStdString(err)).arg(backoffSec));
            sleepOrExit(backoffSec);
            if (shouldExit_.load()) break;
            backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
            continue;
        }
        emit status(QStringLiteral("online — others can connect to %1")
                        .arg(cfg.deviceId));
        backoffSec = 1;

        // Publish the active client so stop() on the GUI thread can close
        // the socket out from under Run() and we unwind cleanly. Cleared
        // again before tearing down so a delayed stop() can't dereference a
        // dead stack frame.
        {
            std::lock_guard<std::mutex> g(clientMu_);
            activeClient_ = &client;
        }

        StreamPump pump(&cap, &client, cfg.fps, cfg.bitrateKbps, cfg.maxHeight);
        pump.Start();

        std::atomic<bool> hbStop{false};
        std::thread hb([&]() {
            while (!hbStop.load()) {
                for (int i = 0; i < 50 && !hbStop.load(); ++i) {
                    std::this_thread::sleep_for(100ms);
                }
                if (hbStop.load()) break;
                std::string e;
                if (!client.SendHeartbeat(&e)) break;
            }
        });

        RelayClient::Callbacks cb;
        cb.onSessionStart = [&]() {
            emit status(QStringLiteral("session active — being controlled"));
            pump.OnSessionStart();
        };
        cb.onSessionEnd = [&]() {
            emit status(QStringLiteral("online — others can connect to %1")
                            .arg(cfg.deviceId));
            pump.OnSessionEnd();
        };
        cb.onInput = [this](std::uint8_t t, const std::uint8_t* d, std::size_t s) {
            // GUI-toggled diagnostic (see setSkipInput). When on, we receive
            // input frames from the viewer normally but drop them before
            // SendInput fires — used to prove whether WeChat Work's alert
            // is triggered by SendInput or by our screen capture path.
            if (skipInput_.load()) return;
            std::string e;
            input::HandleInputFrame(t, d, s, &e);
        };
        cb.onClipboard = [this](const std::uint8_t* d, std::size_t s) {
            QString text = QString::fromUtf8(reinterpret_cast<const char*>(d),
                                              static_cast<int>(s));
            // Hop to GUI thread via queued signal so QClipboard interaction
            // happens on the thread that owns the GUI event loop.
            emit clipboardReceived(text);
        };
        cb.onFileXfer = [this](std::uint8_t type,
                                const std::uint8_t* d, std::size_t s) {
            emit fileXferFrameReceived(
                type,
                QByteArray(reinterpret_cast<const char*>(d), static_cast<int>(s)));
        };
        // UDP_INFO: relay told us session_id + UDP port. Spin up the UDP
        // video transport and hand it to the pump so subsequent
        // SendVideoFrame goes over UDP instead of TCP.
        std::unique_ptr<VideoUdpXport> udpXport;
        cb.onUdpInfo = [&](const std::uint8_t* d, std::size_t s) {
            auto doc = QJsonDocument::fromJson(QByteArray::fromRawData(
                reinterpret_cast<const char*>(d), static_cast<int>(s)));
            if (!doc.isObject()) return;
            const auto obj = doc.object();
            const std::uint32_t sid = static_cast<std::uint32_t>(
                obj.value("session_id").toDouble());
            QString host = obj.value("host").toString();
            const int port = obj.value("port").toInt();
            // Empty host = "reuse the TCP relay address" — the relay's
            // signaling layer deliberately doesn't repeat it.
            if (host.isEmpty()) host = cfg.relayHost;
            if (sid == 0 || port <= 0) return;
            auto x = std::make_unique<VideoUdpXport>();
            std::string e;
            if (!x->Init(host.toStdString(), port, sid,
                          VideoUdpXport::Role::Agent, &e)) {
                std::cerr << "[agent] UDP xport init failed: " << e << "\n";
                return;
            }
            x->SetDirectUpCallback([](const std::string& h, int p) {
                std::cerr << "[agent] UDP direct path up — peer " << h
                          << ":" << p << " (bypassing relay)\n";
            });
            // Agent SENDS video — we don't need an inbound callback (the
            // viewer is the only other peer and it doesn't push frames at
            // us). But we still start the recv loop so the periodic
            // REGISTER inside it keeps the relay map fresh.
            x->StartReceiver([](const std::uint8_t*, std::size_t) {});
            pump.SetUdpXport(x.get());
            udpXport = std::move(x);
            std::cerr << "[agent] UDP video plane up — session_id=" << sid
                      << " relay=" << host.toStdString() << ":" << port << "\n";

            // Advertise our LAN candidates to the peer via the relay's TCP
            // forward. Peer's client will add them to its probe list; if
            // we're same-LAN, one of these will win and we get sub-ms
            // direct without touching any router hairpin.
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
            std::string err2;
            client.SendFrameRaw(proto::MsgUdpMyCandidates,
                                 reinterpret_cast<const std::uint8_t*>(json.constData()),
                                 static_cast<std::size_t>(json.size()), &err2);
            std::cerr << "[agent] advertised " << lanIps.size()
                      << " LAN candidate(s) via TCP\n";
        };
        // Shared parser for BOTH relay-issued reflex candidates and peer-
        // forwarded LAN candidates — payload shape is identical.
        auto addCandidates = [&udpXport](const std::uint8_t* d, std::size_t s,
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
                std::cerr << "[agent] NAT-T probing " << host.toStdString()
                          << ":" << port << " (" << origin << ")\n";
            }
        };
        cb.onUdpCandidates = [addCandidates](const std::uint8_t* d, std::size_t s) {
            addCandidates(d, s, "reflex");
        };
        cb.onUdpPeerLan = [addCandidates](const std::uint8_t* d, std::size_t s) {
            addCandidates(d, s, "lan");
        };
        client.Run(cb);
        // Tear down the UDP xport (if any) before the pump is stopped so
        // it can't be racing a send.
        pump.SetUdpXport(nullptr);
        udpXport.reset();

        {
            std::lock_guard<std::mutex> g(clientMu_);
            activeClient_ = nullptr;
        }

        hbStop.store(true);
        hb.join();
        pump.Stop();
        client.Close();

        if (shouldExit_.load()) break;
        emit status(QStringLiteral("disconnected — reconnecting in %1s")
                        .arg(backoffSec));
        sleepOrExit(backoffSec);
        if (shouldExit_.load()) break;
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }

    emit status(QStringLiteral("agent stopped"));
}
