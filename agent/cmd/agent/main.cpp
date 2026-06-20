// Agent main: capture → encode → push to relay.
//
// Lifecycle:
//   1. Init capture + connect to relay + REGISTER
//   2. Spawn a capture thread that sits idle until SESSION_START
//   3. On SESSION_START: (re)init encoder, then loop capture+encode+send
//   4. On SESSION_END: tear down encoder, return to step 2
//   5. Read loop runs on the main thread; on disconnect the agent exits

#include "capture/dxgi_capturer.h"
#include "encode/h264_encoder.h"
#include "input/input_injector.h"
#include "net/frame.h"
#include "net/relay_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Flags {
    std::string relay = "127.0.0.1:7100";
    std::string deviceId = "device-001";
    std::string deviceToken = "agent-token-001";
    int fps = 30;
    int bitrateKbps = 8000;
};

bool parseFlags(int argc, char** argv, Flags* out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string* v) {
            if (i + 1 >= argc) return false;
            *v = argv[++i];
            return true;
        };
        auto nextInt = [&](int* v) {
            if (i + 1 >= argc) return false;
            *v = std::atoi(argv[++i]);
            return true;
        };
        if (a == "-relay") { if (!next(&out->relay)) return false; }
        else if (a == "-device-id") { if (!next(&out->deviceId)) return false; }
        else if (a == "-device-token") { if (!next(&out->deviceToken)) return false; }
        else if (a == "-fps") { if (!nextInt(&out->fps)) return false; }
        else if (a == "-bitrate") { if (!nextInt(&out->bitrateKbps)) return false; }
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: agent [-relay host:port] [-device-id ID]"
                         " [-device-token TOK] [-fps N] [-bitrate KBPS]\n";
            return false;
        }
        else { std::cerr << "unknown flag: " << a << "\n"; return false; }
    }
    return true;
}

bool splitHostPort(const std::string& s, std::string* host, int* port) {
    const auto pos = s.rfind(':');
    if (pos == std::string::npos) return false;
    *host = s.substr(0, pos);
    *port = std::atoi(s.substr(pos + 1).c_str());
    return *port > 0;
}

// Capture+encode pump. Spends most of its life waiting on `sessionActive`.
class StreamPump {
public:
    StreamPump(DxgiCapturer* cap, RelayClient* client, int fps, int bitrateKbps)
        : cap_(cap), client_(client), fps_(fps), bitrateKbps_(bitrateKbps) {}

    void Start() {
        thread_ = std::thread([this]() { run(); });
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
        {
            std::lock_guard<std::mutex> g(mu_);
            sessionActive_ = true;
        }
        cv_.notify_all();
    }

    void OnSessionEnd() {
        {
            std::lock_guard<std::mutex> g(mu_);
            sessionActive_ = false;
        }
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
        // Wait for a real frame to learn the resolution.
        DxgiCapturer::Frame frame;
        std::string err;
        while (sessionActive_) {
            auto r = cap_->CaptureFrame(100, &frame, &err);
            if (r == DxgiCapturer::Result::kFrame) break;
            if (r == DxgiCapturer::Result::kError) {
                std::cerr << "capture error: " << err << "\n";
                return;
            }
        }
        if (!sessionActive_) return;

        H264Encoder enc;
        H264Encoder::Config ec;
        ec.width = frame.width;
        ec.height = frame.height;
        ec.fps = fps_;
        ec.bitrateKbps = bitrateKbps_;
        // GOP = 1 second's worth of frames. Combined with intra-refresh
        // (configured per-encoder in H264Encoder), this controls how fast a
        // freshly-joined viewer gets a fully-refreshed picture without
        // bursting bandwidth on full IDR frames.
        ec.gopSize = fps_;
        std::string chosen;
        if (!enc.Init(ec, &chosen, &err)) {
            std::cerr << "encoder init failed: " << err << "\n";
            return;
        }
        std::cout << "session started: " << ec.width << "x" << ec.height
                  << " @ " << ec.fps << " fps (" << chosen << ")\n";

        const auto interval = std::chrono::milliseconds(1000 / fps_);

        auto next = std::chrono::steady_clock::now();
        int64_t pts = 0;

        // First frame after SESSION_START is always IDR — encoder Init()
        // does that for us.
        if (!encodeAndSend(enc, frame, pts++, /*forceIdr=*/false, &err)) {
            std::cerr << "first encode/send failed: " << err << "\n";
            return;
        }
        bool wasIdle = false;

        while (sessionActive_) {
            next += interval;
            std::this_thread::sleep_until(next);

            DxgiCapturer::Frame fresh;
            auto r = cap_->CaptureFrame(0, &fresh, &err);
            if (r == DxgiCapturer::Result::kError) {
                std::cerr << "capture error: " << err << "\n";
                return;
            }
            const bool gotNew = (r == DxgiCapturer::Result::kFrame);

            // No DXGI dirty rects → send NOTHING. The relay link stays alive
            // via HEARTBEAT (T2-6), so we don't need to pump video keepalives
            // every 2s the way the old code did. On a truly static screen
            // (reading docs, editor with no input) the link is silent except
            // for the 5s heartbeat.
            if (!gotNew) {
                wasIdle = true;
                continue;
            }

            frame = std::move(fresh);

            // First frame coming out of an idle stretch is forced-IDR so any
            // viewer that lost packets during the silence resyncs on this
            // frame instead of waiting for the next intra-refresh cycle.
            const bool forceIdr = wasIdle;
            if (!encodeAndSend(enc, frame, pts++, forceIdr, &err)) {
                std::cerr << "encode/send failed: " << err << "\n";
                return;
            }
            wasIdle = false;
        }
        std::cout << "session ended\n";
    }

    bool encodeAndSend(H264Encoder& enc, const DxgiCapturer::Frame& f,
                       int64_t pts, bool forceIdr, std::string* err) {
        std::vector<std::vector<std::uint8_t>> pkts;
        if (!enc.EncodeFrame(f.pixels.data(), f.rowPitch, pts, forceIdr, &pkts, err)) {
            return false;
        }
        for (const auto& p : pkts) {
            if (!client_->SendVideoFrame(p.data(), p.size(), err)) return false;
        }
        return true;
    }

    DxgiCapturer* cap_;
    RelayClient* client_;
    int fps_;
    int bitrateKbps_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool sessionActive_ = false;
    bool shouldExit_ = false;
};

} // namespace

int main(int argc, char** argv) {
    Flags flags;
    if (!parseFlags(argc, argv, &flags)) return 1;

    std::string host;
    int port = 0;
    if (!splitHostPort(flags.relay, &host, &port)) {
        std::cerr << "bad -relay value: " << flags.relay << "\n";
        return 1;
    }

    DxgiCapturer cap;
    std::string err;
    if (!cap.Init(&err)) {
        std::cerr << "capture init: " << err << "\n";
        return 2;
    }

    // Reconnect loop. The capturer is reused across attempts; the relay
    // client + stream pump are recreated each time because Run() returning
    // means the socket is in an unknown state.
    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    while (true) {
        RelayClient client;
        RelayClient::Config rc;
        rc.host = host;
        rc.port = port;
        rc.deviceId = flags.deviceId;
        rc.deviceToken = flags.deviceToken;
        if (!client.ConnectAndRegister(rc, &err)) {
            std::cerr << "relay: " << err << " — retrying in "
                      << backoffSec << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
            backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
            continue;
        }
        std::cout << "registered with relay as " << flags.deviceId
                  << ", waiting for session\n";
        backoffSec = 1; // reset backoff after a successful connect

        StreamPump pump(&cap, &client, flags.fps, flags.bitrateKbps);
        pump.Start();

        // Heartbeat sender: every 5s, keeps the relay's idle timer alive and
        // gives us a fast disconnect detection if the link goes away (the
        // next send will fail, the read loop will also bail).
        std::atomic<bool> hbStop{false};
        std::thread hbThread([&]() {
            while (!hbStop.load()) {
                for (int i = 0; i < 50 && !hbStop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (hbStop.load()) break;
                std::string hbErr;
                if (!client.SendHeartbeat(&hbErr)) break;
            }
        });

        RelayClient::Callbacks cb;
        cb.onSessionStart = [&]() { pump.OnSessionStart(); };
        cb.onSessionEnd   = [&]() { pump.OnSessionEnd(); };
        cb.onInput = [](std::uint8_t type, const std::uint8_t* data, std::size_t size) {
            std::string err;
            if (!input::HandleInputFrame(type, data, size, &err)) {
                std::cerr << "drop " << proto::MessageTypeName(type)
                          << " (" << size << "B): " << err << "\n";
            }
        };
        client.Run(cb);

        // Tear down this attempt.
        hbStop.store(true);
        hbThread.join();
        pump.Stop();
        client.Close();

        std::cerr << "disconnected — reconnecting in " << backoffSec << "s\n";
        std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }
}
