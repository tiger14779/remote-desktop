#pragma once

// DXGI Desktop Duplication-based screen capturer.
//
// Init() binds to the primary output of the default adapter and creates a
// duplication. CaptureFrame() returns one frame at a time as BGRA8, top-down.
//
// DXGI only signals new frames on actual screen change. If nothing on screen
// has moved since the previous Acquire, CaptureFrame returns kTimeout — that
// is *not* an error, the caller should just try again.
//
// Two capture modes exist:
//   * CaptureFrame        — legacy CPU path: copies the DXGI texture into a
//                           STAGING resource, maps it, exposes BGRA bytes.
//                           Cost: 1 GPU->CPU readback + 14 MB memcpy at
//                           1440p per frame. Used when the encoder can only
//                           accept CPU-side pixels (libx264 fallback).
//   * CaptureFrameGpu     — zero-copy path: the DXGI texture stays on the
//                           GPU, is copied into an encoder-owned pool
//                           texture with CopySubresourceRegion (GPU-only),
//                           and the encoder reads it directly via NVENC's
//                           AV_PIX_FMT_D3D11 input. No CPU touch, no
//                           swscale. Used when the encoder was Init()'d
//                           with the same D3D11 device we expose here.
//
// The two modes share the same underlying DXGI resources; callers pick per
// frame based on which encoder path is active.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

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

    // Cursor shape, BGRA8, top-down. width*height*4 == bgra.size().
    struct CursorShape {
        int width = 0;
        int height = 0;
        int hotspotX = 0;
        int hotspotY = 0;
        std::vector<std::uint8_t> bgra;
    };

    // If DXGI delivered a new cursor shape in the most recent CaptureFrame
    // call (and the agent hasn't yet picked it up), returns true and copies
    // the decoded BGRA into *out. Subsequent calls return false until a
    // new shape is available. Mono / masked-color shapes are pre-converted
    // here so the wire format stays a single BGRA blob.
    bool TakeCursorShape(CursorShape* out);

    // GPU-direct capture. Populates out->width/height/timestamp and out->
    // gpuTexture (holds a ComPtr internally); pixels is left empty. The
    // texture pointer is only guaranteed valid until the NEXT CaptureFrame*
    // call — the caller must consume it (encoder feeds it to NVENC) before
    // then. Same timing / kTimeout semantics as CaptureFrame.
    struct GpuFrame {
        int width = 0;
        int height = 0;
        std::chrono::system_clock::time_point timestamp;
        // Non-owning pointer into DxgiCapturer's own texture pool. Do NOT
        // Release; the capturer manages lifetime.
        ID3D11Texture2D* gpuTexture = nullptr;
    };
    Result CaptureFrameGpu(int timeoutMs, GpuFrame* out, std::string* errorOut);

    // Accessors so the encoder can share our D3D11 device (must match for
    // textures to be readable). Returns nullptr before Init().
    ID3D11Device*        D3D11Device()  const;
    ID3D11DeviceContext* D3D11Context() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool createDevice(std::string* errorOut);
    bool createDuplication(std::string* errorOut);
    bool ensureStaging(int width, int height, std::string* errorOut);
    // Ensures gpuPool_ contains a size/format-matching texture we can
    // CopySubresourceRegion the DXGI-acquired frame into; the encoder then
    // reads FROM that pool texture. We can't hand the DXGI-acquired
    // texture directly to the encoder because DXGI requires ReleaseFrame
    // before the next Acquire, and the encoder is still holding the
    // reference during avcodec_send_frame.
    bool ensureGpuPool(int width, int height, std::string* errorOut);
};
