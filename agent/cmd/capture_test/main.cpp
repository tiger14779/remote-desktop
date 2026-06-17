// Phase 2a smoke test: initialise the capturer and write one frame to a BMP.
// On idle desktops DXGI may report timeout for many tries — we poll until we
// see a real change or hit a hard cap.

#include "capture/dxgi_capturer.h"
#include "util/bmp_writer.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string outPath = "capture.bmp";
    if (argc >= 2) outPath = argv[1];

    DxgiCapturer cap;
    std::string err;
    if (!cap.Init(&err)) {
        std::cerr << "init failed: " << err << "\n";
        return 1;
    }
    std::cout << "capturer ready; waiting for a screen change...\n";

    DxgiCapturer::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int timeouts = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = cap.CaptureFrame(200, &frame, &err);
        if (r == DxgiCapturer::Result::kFrame) {
            std::cout << "got frame " << frame.width << "x" << frame.height
                      << " (rowPitch=" << frame.rowPitch
                      << ", pixels=" << frame.pixels.size() << " bytes)\n";
            if (!WriteBmp(outPath, frame.width, frame.height,
                          frame.rowPitch, frame.pixels.data())) {
                std::cerr << "writing " << outPath << " failed\n";
                return 2;
            }
            std::cout << "wrote " << outPath << "\n";
            return 0;
        }
        if (r == DxgiCapturer::Result::kError) {
            std::cerr << "capture error: " << err << "\n";
            return 3;
        }
        // kTimeout: idle desktop, keep polling.
        if (++timeouts % 25 == 0) {
            std::cout << "no screen change yet... (move the mouse or click something)\n";
        }
    }
    std::cerr << "no frame within 10s — is the desktop completely idle?\n";
    return 4;
}
