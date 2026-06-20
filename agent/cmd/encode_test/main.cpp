// Phase 2b smoke test: capture frames, encode them to H.264, and dump the raw
// Annex-B stream to a .h264 file. Open the result with ffplay to verify.
//
// Usage: encode_test [out.h264] [seconds]
//
// The encoder is sized to the first captured frame's dimensions, so this works
// at native resolution (incl. 4K) without any setup.

#include "capture/dxgi_capturer.h"
#include "encode/h264_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    std::string outPath = "out.h264";
    int seconds = 5;
    if (argc >= 2) outPath = argv[1];
    if (argc >= 3) seconds = std::max(1, std::atoi(argv[2]));

    DxgiCapturer cap;
    std::string err;
    if (!cap.Init(&err)) {
        std::cerr << "capture init: " << err << "\n";
        return 1;
    }

    // Wait for the first real frame so we know the resolution.
    DxgiCapturer::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        auto r = cap.CaptureFrame(200, &frame, &err);
        if (r == DxgiCapturer::Result::kFrame) break;
        if (r == DxgiCapturer::Result::kError) {
            std::cerr << "capture error: " << err << "\n";
            return 1;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "timed out waiting for first frame\n";
            return 1;
        }
    }
    std::cout << "first frame: " << frame.width << "x" << frame.height << "\n";

    H264Encoder enc;
    H264Encoder::Config ec;
    ec.width = frame.width;
    ec.height = frame.height;
    ec.fps = 30;
    ec.bitrateKbps = 8000;
    ec.gopSize = 60;

    std::string chosen;
    if (!enc.Init(ec, &chosen, &err)) {
        std::cerr << "encoder init: " << err << "\n";
        return 2;
    }
    std::cout << "encoder: " << chosen << "  (" << ec.width << "x" << ec.height
              << " @ " << ec.fps << "fps, " << ec.bitrateKbps << " kbps)\n";

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "cannot open " << outPath << " for write\n";
        return 3;
    }

    auto encodeFirst = [&]() {
        std::vector<std::vector<uint8_t>> pkts;
        if (!enc.EncodeFrame(frame.pixels.data(), frame.rowPitch, 0, /*forceIdr=*/false, &pkts, &err)) {
            std::cerr << "encode: " << err << "\n";
            return false;
        }
        for (auto& p : pkts) out.write(reinterpret_cast<const char*>(p.data()), p.size());
        return true;
    };
    if (!encodeFirst()) return 4;

    const int totalFrames = seconds * ec.fps;
    const auto frameInterval = std::chrono::milliseconds(1000 / ec.fps);
    auto next = std::chrono::steady_clock::now() + frameInterval;

    int64_t pts = 1;
    int captured = 1, encoded = 1;
    size_t totalBytes = 0;

    while (pts < totalFrames) {
        std::this_thread::sleep_until(next);
        next += frameInterval;

        // Drain capturer; if no new frame is available, re-encode the previous
        // one so we hit the target fps and the resulting stream is uniform.
        DxgiCapturer::Frame fresh;
        auto r = cap.CaptureFrame(0, &fresh, &err);
        if (r == DxgiCapturer::Result::kFrame) {
            frame = std::move(fresh);
            captured++;
        } else if (r == DxgiCapturer::Result::kError) {
            std::cerr << "capture error: " << err << "\n";
            return 1;
        }

        std::vector<std::vector<uint8_t>> pkts;
        if (!enc.EncodeFrame(frame.pixels.data(), frame.rowPitch, pts, /*forceIdr=*/false, &pkts, &err)) {
            std::cerr << "encode: " << err << "\n";
            return 4;
        }
        for (auto& p : pkts) {
            out.write(reinterpret_cast<const char*>(p.data()), p.size());
            totalBytes += p.size();
        }
        encoded++;
        pts++;
    }

    // Drain.
    std::vector<std::vector<uint8_t>> pkts;
    if (!enc.Flush(&pkts, &err)) {
        std::cerr << "flush: " << err << "\n";
        return 5;
    }
    for (auto& p : pkts) {
        out.write(reinterpret_cast<const char*>(p.data()), p.size());
        totalBytes += p.size();
    }

    std::cout << "wrote " << outPath << "  (" << totalBytes << " bytes, "
              << encoded << " encoded frames; " << captured
              << " came from actual screen change)\n";
    std::cout << "verify:  ffplay -f h264 " << outPath << "\n";
    return 0;
}
