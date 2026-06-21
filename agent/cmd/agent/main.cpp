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
#include "net/agent_relay_client.h"

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
    // Cap on the encoded vertical resolution. 4K screens get downscaled to
    // this before encoding — viewer sees and decodes the smaller image, so
    // every stage of the pipeline (encode CPU, network, decode, paint) does
    // 1/3 the work versus native 4K. 0 means no cap.
    int maxHeight = 1440;
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
        else if (a == "-max-height") { if (!nextInt(&out->maxHeight)) return false; }
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: agent [-relay host:port] [-device-id ID]"
                         " [-device-token TOK] [-fps N] [-bitrate KBPS]"
                         " [-max-height N (default 1440 = 2K; 0 = native)]\n";
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
    StreamPump(DxgiCapturer* cap, RelayClient* client, int fps,
               int bitrateKbps, int maxHeight)
        : cap_(cap), client_(client), fps_(fps),
          bitrateKbps_(bitrateKbps), maxHeight_(maxHeight) {}

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
        ec.srcWidth  = frame.width;
        ec.srcHeight = frame.height;
        // Compute encoder output dims: scale source down to fit the maxHeight
        // cap while preserving aspect ratio. H.264 + YUV420 require both
        // dimensions to be even.
        if (maxHeight_ > 0 && frame.height > maxHeight_) {
            ec.height = maxHeight_;
            ec.width  = static_cast<int>(
                static_cast<int64_t>(frame.width) * maxHeight_ / frame.height);
            ec.width  &= ~1;
            ec.height &= ~1;
        } else {
            ec.width  = frame.width  & ~1;
            ec.height = frame.height & ~1;
        }
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
        std::cout << "session started: " << ec.srcWidth << "x" << ec.srcHeight
                  << " -> " << ec.width << "x" << ec.height
                  << " @ " << ec.fps << " fps (" << chosen << ")\n";

        const auto interval = std::chrono::milliseconds(1000 / fps_);

        auto next = std::chrono::steady_clock::now();
        int64_t pts = 0;

        // First frame after SESSION_START is always IDR — encoder Init()
        // does that for us.
        std::size_t firstBytes = 0;
        if (!encodeAndSend(enc, frame, pts++, /*forceIdr=*/false, &firstBytes, &err)) {
            std::cerr << "first encode/send failed: " << err << "\n";
            return;
        }
        bool wasIdle = false;
        auto lastGotFrame = std::chrono::steady_clock::now();
        // Only mark wasIdle (and force the next frame to IDR) after sustained
        // silence. Short kTimeout gaps between active frames — like the
        // mouse-only updates DXGI returns when dragging a window — would
        // otherwise flap this flag on every other iter and emit a 250+ KB
        // IDR several times per second, clogging the network and tanking
        // perceived latency.
        constexpr auto kIdleThreshold = std::chrono::seconds(1);

        // Periodic + per-frame diagnostics. Surfaces the cmd→txt scene
        // change case: a huge encoded frame or a slow encode call is what
        // turns an instant flip into a perceived multi-second lag.
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
                // Don't kill the session — DXGI can transiently lose access
                // (lockscreen, UAC, GPU reset) and we'd rather just keep
                // trying until it comes back. Full Init() rebuilds device +
                // duplication, which covers the GPU-reset case the lazy
                // recovery in CaptureFrame can't.
                std::cerr << "capture error: " << err << " — reinit\n";
                wasIdle = true;
                std::string initErr;
                if (!cap_->Init(&initErr)) {
                    std::cerr << "capture reinit failed: " << initErr
                              << " — retry in 500ms\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                continue;
            }
            const bool gotNew = (r == DxgiCapturer::Result::kFrame);

            // No DXGI dirty rects → send NOTHING. The relay link stays alive
            // via HEARTBEAT (T2-6), so we don't need to pump video keepalives
            // every 2s the way the old code did. On a truly static screen
            // (reading docs, editor with no input) the link is silent except
            // for the 5s heartbeat.
            if (!gotNew) {
                if (std::chrono::steady_clock::now() - lastGotFrame > kIdleThreshold) {
                    wasIdle = true;
                }
                continue;
            }

            frame = std::move(fresh);
            lastGotFrame = std::chrono::steady_clock::now();

            // First frame coming out of an idle stretch is forced-IDR so any
            // viewer that lost packets during the silence resyncs on this
            // frame instead of waiting for the next intra-refresh cycle.
            const bool forceIdr = wasIdle;
            std::size_t thisBytes = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (!encodeAndSend(enc, frame, pts++, forceIdr, &thisBytes, &err)) {
                std::cerr << "encode/send failed: " << err << "\n";
                return;
            }
            const auto t1 = std::chrono::steady_clock::now();
            const long long encMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            wasIdle = false;

            // Per-frame warning: any frame that takes longer than half a
            // frame slot (50ms ≈ 1.5 slots at 30fps) or emits more than
            // 200KB is what drives the user-visible scene-change lag.
            if (encMs > 50 || thisBytes > 200 * 1024) {
                std::cerr << "[stat] heavy frame: " << encMs << "ms, "
                          << (thisBytes / 1024) << "KB"
                          << (forceIdr ? " (forced IDR)" : "") << "\n";
            }

            statFrames++;
            statBytes += thisBytes;
            if (encMs > statMaxMs) statMaxMs = encMs;
            if (thisBytes > statMaxBytes) statMaxBytes = thisBytes;
            const auto since = std::chrono::duration_cast<std::chrono::seconds>(t1 - statStart).count();
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
        std::cout << "session ended\n";
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
    int fps_;
    int bitrateKbps_;
    int maxHeight_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool sessionActive_ = false;
    bool shouldExit_ = false;
};

} // namespace

int main(int argc, char** argv) {
    // Make std::cout flush per-write. PowerShell pipes a native process's
    // stdout through itself, which Windows libc treats as "fully buffered"
    // (no flush until 4 KB or exit). Without this, status messages can
    // pile up invisibly while the user thinks the agent is hung.
    std::cout.setf(std::ios::unitbuf);

    Flags flags;
    if (!parseFlags(argc, argv, &flags)) return 1;

    // Echo the resolved config to stderr right away — cerr is unbuffered,
    // so this is the FIRST thing the operator sees regardless of buffering.
    // Catches "I pasted with backticks into cmd instead of PowerShell and
    // got the wrong defaults" before it wastes 30s of confusion.
    std::cerr << "[agent] starting"
              << " relay=" << flags.relay
              << " device-id=" << flags.deviceId
              << " token=" << (flags.deviceToken.size() > 8
                                   ? flags.deviceToken.substr(0, 8) + "..."
                                   : flags.deviceToken)
              << " fps=" << flags.fps
              << " bitrate=" << flags.bitrateKbps << "kbps"
              << " max-height=" << flags.maxHeight << "\n";

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
        std::cerr << "[agent] dialing " << host << ":" << port << "...\n";
        if (!client.ConnectAndRegister(rc, &err)) {
            std::cerr << "[agent] relay: " << err << " — retrying in "
                      << backoffSec << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
            backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
            continue;
        }
        std::cerr << "[agent] registered as " << flags.deviceId
                  << ", waiting for session\n";
        backoffSec = 1; // reset backoff after a successful connect

        StreamPump pump(&cap, &client, flags.fps, flags.bitrateKbps, flags.maxHeight);
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
