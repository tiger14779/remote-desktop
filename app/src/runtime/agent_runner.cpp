#include "runtime/agent_runner.h"

#include "capture/dxgi_capturer.h"
#include "encode/h264_encoder.h"
#include "input/input_injector.h"
#include "net/frame.h"
#include "net/agent_relay_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
        std::string chosen;
        if (!enc.Init(ec, &chosen, &err)) {
            std::cerr << "[agent] encoder init failed: " << err << "\n";
            return;
        }
        std::cerr << "[agent] session: " << ec.srcWidth << "x" << ec.srcHeight
                  << " -> " << ec.width << "x" << ec.height
                  << " @ " << ec.fps << "fps (" << chosen << ")\n";

        const auto interval = std::chrono::milliseconds(1000 / fps_);
        auto next = std::chrono::steady_clock::now();
        int64_t pts = 0;
        std::size_t firstBytes = 0;
        if (!encodeAndSend(enc, frame, pts++, /*forceIdr=*/false,
                           &firstBytes, &err)) return;
        bool wasIdle = false;

        // Per-frame + per-2s diagnostics. Surfaces "encode too slow" and
        // "scene change emits a giant frame" as the actual lag culprits.
        auto statStart = std::chrono::steady_clock::now();
        int statFrames = 0;
        std::size_t statBytes = 0;
        long long statMaxMs = 0;
        std::size_t statMaxBytes = 0;

        while (sessionActive_) {
            next += interval;
            std::this_thread::sleep_until(next);

            DxgiCapturer::Frame fresh;
            auto r = cap_->CaptureFrame(0, &fresh, &err);
            if (r == DxgiCapturer::Result::kError) {
                std::cerr << "[agent] capture error: " << err << " — reinit\n";
                if (!cap_->Init(&err)) std::this_thread::sleep_for(500ms);
                wasIdle = true;
                continue;
            }
            if (r != DxgiCapturer::Result::kFrame) { wasIdle = true; continue; }
            frame = std::move(fresh);

            const bool forceIdr = wasIdle;
            std::size_t thisBytes = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (!encodeAndSend(enc, frame, pts++, forceIdr, &thisBytes, &err)) return;
            const auto t1 = std::chrono::steady_clock::now();
            const long long encMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            wasIdle = false;

            // Per-frame warning. Scene-change forced IDR + slow encode is
            // the usual fingerprint for the click-to-display lag spike.
            if (encMs > 50 || thisBytes > 200 * 1024) {
                std::cerr << "[stat] heavy frame: " << encMs << "ms, "
                          << (thisBytes / 1024) << "KB"
                          << (forceIdr ? " (forced IDR)" : "") << "\n";
            }
            statFrames++;
            statBytes += thisBytes;
            if (encMs > statMaxMs) statMaxMs = encMs;
            if (thisBytes > statMaxBytes) statMaxBytes = thisBytes;
            const auto since =
                std::chrono::duration_cast<std::chrono::seconds>(t1 - statStart).count();
            if (since >= 2) {
                std::cerr << "[stat] " << statFrames << " frames in " << since
                          << "s, total " << (statBytes / 1024) << "KB"
                          << ", max-enc " << statMaxMs << "ms"
                          << ", max-frame " << (statMaxBytes / 1024) << "KB\n";
                statStart = t1;
                statFrames = 0;
                statBytes = 0;
                statMaxMs = 0;
                statMaxBytes = 0;
            }
        }
    }

    bool encodeAndSend(H264Encoder& enc, const DxgiCapturer::Frame& f,
                       int64_t pts, bool forceIdr,
                       std::size_t* bytesOut, std::string* err) {
        std::vector<std::vector<std::uint8_t>> pkts;
        if (!enc.EncodeFrame(f.pixels.data(), f.rowPitch, pts, forceIdr, &pkts, err)) {
            return false;
        }
        std::size_t total = 0;
        for (const auto& p : pkts) {
            if (!client_->SendVideoFrame(p.data(), p.size(), err)) return false;
            total += p.size();
        }
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
        cb.onInput = [](std::uint8_t t, const std::uint8_t* d, std::size_t s) {
            std::string e;
            input::HandleInputFrame(t, d, s, &e);
        };
        client.Run(cb);

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
