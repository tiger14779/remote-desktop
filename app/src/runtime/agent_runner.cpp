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
            std::size_t bytes = 0;
            if (!encodeAndSend(enc, frame, pts++, wasIdle, &bytes, &err)) return;
            wasIdle = false;
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
            std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
            backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
            continue;
        }
        emit status(QStringLiteral("online — others can connect to %1")
                        .arg(cfg.deviceId));
        backoffSec = 1;

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

        hbStop.store(true);
        hb.join();
        pump.Stop();
        client.Close();

        if (shouldExit_.load()) break;
        emit status(QStringLiteral("disconnected — reconnecting in %1s")
                        .arg(backoffSec));
        std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }

    emit status(QStringLiteral("agent stopped"));
}
