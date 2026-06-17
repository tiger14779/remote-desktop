#pragma once

// DXGI Desktop Duplication-based screen capturer.
//
// Init() binds to the primary output of the default adapter and creates a
// duplication. CaptureFrame() returns one frame at a time as BGRA8, top-down.
//
// DXGI only signals new frames on actual screen change. If nothing on screen
// has moved since the previous Acquire, CaptureFrame returns kTimeout — that
// is *not* an error, the caller should just try again.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class DxgiCapturer {
public:
    DxgiCapturer();
    ~DxgiCapturer();

    DxgiCapturer(const DxgiCapturer&) = delete;
    DxgiCapturer& operator=(const DxgiCapturer&) = delete;

    bool Init(std::string* errorOut);

    struct Frame {
        int width = 0;
        int height = 0;
        int rowPitch = 0; // bytes per row; may exceed width*4
        std::vector<uint8_t> pixels; // BGRA8, top-down. Size == rowPitch * height.
        std::chrono::system_clock::time_point timestamp;
    };

    enum class Result { kFrame, kTimeout, kError };

    Result CaptureFrame(int timeoutMs, Frame* out, std::string* errorOut);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool createDevice(std::string* errorOut);
    bool createDuplication(std::string* errorOut);
    bool ensureStaging(int width, int height, std::string* errorOut);
};
