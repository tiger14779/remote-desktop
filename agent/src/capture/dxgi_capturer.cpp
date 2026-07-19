#include "capture/dxgi_capturer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
std::string hresultMsg(const char* what, HRESULT hr) {
    std::stringstream ss;
    ss << what << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}

// Decodes a raw DXGI cursor shape into top-down BGRA. Three input formats
// share this path; the cursor we send the viewer is always the same flat
// BGRA blob so the rendering side doesn't need to know about DXGI's wire
// format quirks.
void decodeCursorShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                       const std::uint8_t* buf, std::size_t /*bufSize*/,
                       DxgiCapturer::CursorShape* out) {
    out->hotspotX = static_cast<int>(info.HotSpot.x);
    out->hotspotY = static_cast<int>(info.HotSpot.y);

    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // Already BGRA in row-major top-down. Pitch may exceed width*4.
        out->width  = static_cast<int>(info.Width);
        out->height = static_cast<int>(info.Height);
        out->bgra.assign(static_cast<std::size_t>(out->width) * out->height * 4, 0);
        for (int y = 0; y < out->height; ++y) {
            std::memcpy(&out->bgra[static_cast<std::size_t>(y) * out->width * 4],
                        &buf[static_cast<std::size_t>(y) * info.Pitch],
                        static_cast<std::size_t>(out->width) * 4);
        }
    } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // BGRA where the alpha byte is a mask:
        //   alpha=0x00 -> opaque cursor pixel, use BGR directly
        //   alpha=0xFF -> XOR with the underlying screen (we have no screen
        //                 to XOR client-side, so fall to transparent — same
        //                 reasoning as the monochrome invert case)
        out->width  = static_cast<int>(info.Width);
        out->height = static_cast<int>(info.Height);
        out->bgra.assign(static_cast<std::size_t>(out->width) * out->height * 4, 0);
        for (int y = 0; y < out->height; ++y) {
            for (int x = 0; x < out->width; ++x) {
                const std::uint8_t* src =
                    &buf[static_cast<std::size_t>(y) * info.Pitch + x * 4];
                std::uint8_t* dst =
                    &out->bgra[(static_cast<std::size_t>(y) * out->width + x) * 4];
                if (src[3] == 0xFF) {
                    dst[0] = 0; dst[1] = 0; dst[2] = 0; dst[3] = 0;
                } else {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
                }
            }
        }
    } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Buffer = two stacked 1-bit-per-pixel bitmaps of equal height:
        // [AND mask | XOR mask], each Height/2 rows tall.
        //   AND=0, XOR=0 -> opaque BLACK   (cursor pixel)
        //   AND=0, XOR=1 -> opaque WHITE   (cursor pixel)
        //   AND=1, XOR=0 -> transparent    (background shows through)
        //   AND=1, XOR=1 -> INVERT (XOR'd with screen). We have no screen
        //     to XOR against and rendering it as solid white was creating
        //     a big white box on the I-beam that obscured the underlying
        //     text. Make it transparent: visually it just means the cursor
        //     loses its anti-background contrast halo, but the shape's
        //     opaque black/white portion still draws fine.
        out->width  = static_cast<int>(info.Width);
        out->height = static_cast<int>(info.Height) / 2;
        out->bgra.assign(static_cast<std::size_t>(out->width) * out->height * 4, 0);
        for (int y = 0; y < out->height; ++y) {
            for (int x = 0; x < out->width; ++x) {
                const auto andByte =
                    buf[static_cast<std::size_t>(y) * info.Pitch + (x / 8)];
                const auto xorByte =
                    buf[static_cast<std::size_t>(y + out->height) * info.Pitch + (x / 8)];
                const int shift = 7 - (x % 8);
                const bool andBit = (andByte >> shift) & 1;
                const bool xorBit = (xorByte >> shift) & 1;
                std::uint8_t* dst =
                    &out->bgra[(static_cast<std::size_t>(y) * out->width + x) * 4];
                if (!andBit && !xorBit) {
                    dst[0]=0;   dst[1]=0;   dst[2]=0;   dst[3]=255;  // black
                } else if (!andBit && xorBit) {
                    dst[0]=255; dst[1]=255; dst[2]=255; dst[3]=255;  // white
                } else {
                    // andBit == 1 (transparent or invert). Both rendered as
                    // fully transparent — see comment above.
                    dst[0]=0; dst[1]=0; dst[2]=0; dst[3]=0;
                }
            }
        }
    } else {
        // Unknown type — emit an empty shape so the viewer reverts to its
        // platform default rather than rendering garbage.
        out->width = 0;
        out->height = 0;
        out->bgra.clear();
    }
}
} // namespace

struct DxgiCapturer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    // GPU-direct path pool: the encoder's hw_frames_ctx wants a
    // driver-friendly texture (DEFAULT usage, no CPU access, no map). We
    // copy the DXGI-acquired frame into this pool texture with
    // CopySubresourceRegion and hand it to the encoder. Reused across
    // frames so we're not allocating textures per capture.
    ComPtr<ID3D11Texture2D> gpuPool;
    int gpuPoolW = 0;
    int gpuPoolH = 0;
    DXGI_OUTPUT_DESC outputDesc{};
    bool frameHeld = false; // true between AcquireNextFrame and ReleaseFrame

    // Pending cursor shape filled in by CaptureFrame, drained by
    // TakeCursorShape. hasPending_ resets to false after a take so we don't
    // re-send the same shape — DXGI signals shape changes via a non-zero
    // PointerShapeBufferSize in DXGI_OUTDUPL_FRAME_INFO.
    DxgiCapturer::CursorShape pendingCursor;
    bool hasPendingCursor = false;
    std::vector<std::uint8_t> shapeBuf; // reusable raw buffer for GetFramePointerShape
};

DxgiCapturer::DxgiCapturer() : impl_(std::make_unique<Impl>()) {}

DxgiCapturer::~DxgiCapturer() {
    if (impl_->frameHeld && impl_->duplication) {
        impl_->duplication->ReleaseFrame();
    }
}

bool DxgiCapturer::Init(std::string* errorOut) {
    // Combined path: for each adapter (integrated + discrete on a hybrid
    // laptop, single dGPU on desktops), create a D3D11 device tied to
    // THAT adapter, then try DuplicateOutput on each of its outputs. Pick
    // the first (device, output) pair that succeeds.
    //
    // Motivation: D3D11CreateDevice(nullptr) picks whatever adapter the
    // driver decides is "best" — typically the discrete GPU on Optimus
    // laptops. But the physical monitor is often plugged into the iGPU's
    // outputs, so DuplicateOutput on the dGPU's device against the iGPU's
    // output returns E_INVALIDARG (0x80070057, "specified device is not
    // the same device that was used to create the output resource"). The
    // fix is to walk everything and pair a device with an output on the
    // SAME adapter.
    return createAndDuplicate(errorOut);
}

bool DxgiCapturer::createDevice(std::string* errorOut) {
    // Kept as a fallback / for callers who only need the D3D11 device.
    return createAndDuplicate(errorOut);
}

bool DxgiCapturer::createDuplication(std::string* /*errorOut*/) {
    // Consolidated into createAndDuplicate. This entry point stays for
    // backward compat with older callers; a null impl just means "already
    // handled by Init()".
    return impl_->duplication != nullptr;
}

bool DxgiCapturer::createAndDuplicate(std::string* errorOut) {
    // Start from a clean slate. Critical for DEVICE_REMOVED (0x887a0005,
    // driver reset/TDR) recovery:
    //   (a) the old duplication must be RELEASED before DuplicateOutput —
    //       DXGI allows one duplication per output, so a live stale one
    //       makes every re-init fail (E_ACCESSDENIED) forever;
    //   (b) staging/gpuPool belong to the old device. ensureStaging reuses
    //       them on size match, CopyResource across devices is a silent
    //       no-op (returns void), and Map on the dead device's texture
    //       returns DEVICE_REMOVED — an infinite "reinit" loop that never
    //       actually recovers.
    if (impl_->frameHeld && impl_->duplication) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
    }
    impl_->duplication.Reset();
    impl_->staging.Reset();
    impl_->gpuPool.Reset();
    impl_->gpuPoolW = 0;
    impl_->gpuPoolH = 0;
    impl_->context.Reset();
    impl_->device.Reset();

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    // BGRA_SUPPORT is required for Desktop Duplication on some driver
    // versions (Intel iGPU in particular); harmless everywhere else.
    const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("CreateDXGIFactory1", hr);
        return false;
    }

    std::string lastErr = "no adapter/output pair produced a working DuplicateOutput";
    for (UINT ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 adesc{};
        adapter->GetDesc1(&adesc);
        // Software (WARP) adapter — skip; DuplicateOutput on WARP works
        // but produces a black frame in most Windows configs.
        if (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // Create a D3D11 device pinned to THIS adapter.
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL got;
        hr = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            deviceFlags, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            device.GetAddressOf(), &got, context.GetAddressOf());
        if (FAILED(hr)) {
            const std::string ctx = "D3D11CreateDevice(adapter#" +
                                    std::to_string(ai) + ")";
            lastErr = hresultMsg(ctx.c_str(), hr);
            continue;
        }

        // Walk this adapter's outputs, try DuplicateOutput on each.
        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, output.GetAddressOf())
                == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC odesc{};
            output->GetDesc(&odesc);
            // Only care about outputs that are actually attached to the
            // desktop — a headless dGPU on a laptop won't have any.
            if (!odesc.AttachedToDesktop) continue;

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) continue;

            ComPtr<IDXGIOutputDuplication> dup;
            hr = output1->DuplicateOutput(device.Get(), dup.GetAddressOf());
            if (SUCCEEDED(hr)) {
                impl_->device      = std::move(device);
                impl_->context     = std::move(context);
                impl_->duplication = std::move(dup);
                impl_->outputDesc  = odesc;
                std::cerr << "[dxgi] capture bound to adapter#" << ai
                          << " output#" << oi
                          << " (" << odesc.DesktopCoordinates.right -
                                     odesc.DesktopCoordinates.left
                          << "x" << odesc.DesktopCoordinates.bottom -
                                     odesc.DesktopCoordinates.top << ")\n";
                return true;
            }
            const std::string ctx = "DuplicateOutput(adapter#" +
                                    std::to_string(ai) + " output#" +
                                    std::to_string(oi) + ")";
            lastErr = hresultMsg(ctx.c_str(), hr);
        }
    }

    if (errorOut) *errorOut = lastErr;
    return false;
}

bool DxgiCapturer::ensureStaging(int width, int height, std::string* errorOut) {
    if (impl_->staging) {
        D3D11_TEXTURE2D_DESC desc;
        impl_->staging->GetDesc(&desc);
        if (static_cast<int>(desc.Width) == width &&
            static_cast<int>(desc.Height) == height) {
            return true;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    impl_->staging.Reset();
    HRESULT hr = impl_->device->CreateTexture2D(&desc, nullptr, impl_->staging.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("CreateTexture2D(staging)", hr);
        return false;
    }
    return true;
}

DxgiCapturer::Result DxgiCapturer::CaptureFrame(int timeoutMs, Frame* out, std::string* errorOut) {
    if (!impl_->duplication) {
        // Lazy recovery from a previous DXGI_ERROR_ACCESS_LOST (resolution
        // change, UAC prompt, Win+L lockscreen, driver restart, sleep/wake).
        // If the D3D11 device itself is also gone (GPU reset), the caller
        // has to fall back to a full Init() — handled in agent main.
        if (!impl_->device) {
            if (errorOut) *errorOut = "capturer not initialised";
            return Result::kError;
        }
        if (!createDuplication(errorOut)) {
            return Result::kError;
        }
    }

    if (impl_->frameHeld) {
        // Defensive: should never trigger if every code path Releases below.
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = impl_->duplication->AcquireNextFrame(
        static_cast<UINT>(timeoutMs), &info, res.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Result::kTimeout;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Resolution change, display unplug, etc. Caller must Init() again.
        impl_->duplication.Reset();
        if (errorOut) *errorOut = "DXGI access lost; reinit required";
        return Result::kError;
    }
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("AcquireNextFrame", hr);
        return Result::kError;
    }
    impl_->frameHeld = true;

    // Cursor shape pulled here while we still hold the frame — DXGI only
    // permits GetFramePointerShape between AcquireNextFrame and ReleaseFrame.
    // PointerShapeBufferSize is non-zero exactly when a new shape has been
    // committed since the previous Acquire; otherwise DXGI returns nothing.
    if (info.PointerShapeBufferSize > 0) {
        if (impl_->shapeBuf.size() < info.PointerShapeBufferSize) {
            impl_->shapeBuf.resize(info.PointerShapeBufferSize);
        }
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
        UINT used = 0;
        HRESULT hr2 = impl_->duplication->GetFramePointerShape(
            info.PointerShapeBufferSize,
            impl_->shapeBuf.data(),
            &used,
            &shapeInfo);
        if (SUCCEEDED(hr2) && used > 0) {
            decodeCursorShape(shapeInfo, impl_->shapeBuf.data(), used,
                              &impl_->pendingCursor);
            if (!impl_->pendingCursor.bgra.empty()) {
                impl_->hasPendingCursor = true;
            }
        }
    }

    // We no longer filter on LastPresentTime==0. Previously we treated
    // cursor-only deltas as "no useful frame", but the side effect was:
    //  (a) at session start, if the agent desktop was idle, the viewer
    //      stayed black until something changed.
    //  (b) at the END of a fast drag, if the very last present landed
    //      between our polls, the only frames left in the pipeline were
    //      stale mid-drag ones — the window appeared "stuck in the middle"
    //      until the user wiggled the mouse to force a new present.
    // The cost of NOT filtering is occasional tiny P-frames for cursor-
    // only updates; on NVENC + LAN that's negligible.

    ComPtr<ID3D11Texture2D> captured;
    hr = res.As(&captured);
    if (FAILED(hr)) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        if (errorOut) *errorOut = hresultMsg("QI ID3D11Texture2D", hr);
        return Result::kError;
    }

    D3D11_TEXTURE2D_DESC capDesc;
    captured->GetDesc(&capDesc);

    if (!ensureStaging(static_cast<int>(capDesc.Width),
                       static_cast<int>(capDesc.Height), errorOut)) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        return Result::kError;
    }

    impl_->context->CopyResource(impl_->staging.Get(), captured.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = impl_->context->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        if (errorOut) *errorOut = hresultMsg("Map(staging)", hr);
        return Result::kError;
    }

    out->width = static_cast<int>(capDesc.Width);
    out->height = static_cast<int>(capDesc.Height);
    out->rowPitch = static_cast<int>(mapped.RowPitch);
    const size_t bytes = static_cast<size_t>(mapped.RowPitch) * capDesc.Height;
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    out->pixels.assign(src, src + bytes);
    out->timestamp = std::chrono::system_clock::now();

    impl_->context->Unmap(impl_->staging.Get(), 0);
    impl_->duplication->ReleaseFrame();
    impl_->frameHeld = false;
    return Result::kFrame;
}

bool DxgiCapturer::TakeCursorShape(CursorShape* out) {
    if (!impl_->hasPendingCursor) return false;
    *out = impl_->pendingCursor;
    impl_->hasPendingCursor = false;
    return true;
}

ID3D11Device* DxgiCapturer::D3D11Device() const {
    return impl_->device.Get();
}

ID3D11DeviceContext* DxgiCapturer::D3D11Context() const {
    return impl_->context.Get();
}

bool DxgiCapturer::ensureGpuPool(int width, int height, std::string* errorOut) {
    if (impl_->gpuPool && impl_->gpuPoolW == width && impl_->gpuPoolH == height) {
        return true;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // DXGI Desktop Duplication yields BGRA; NVENC via ffmpeg reads either
    // BGRA (AV_PIX_FMT_BGRA) or NV12 directly. Keep BGRA — NVENC does the
    // internal color conversion for free on the GPU.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // BIND_SHADER_RESOURCE is what ffmpeg's NVENC binding needs so it can
    // sample from the texture. RENDER_TARGET makes future in-shader
    // resize / cursor overlay paths possible without recreating.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = 0;

    impl_->gpuPool.Reset();
    HRESULT hr = impl_->device->CreateTexture2D(
        &desc, nullptr, impl_->gpuPool.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("CreateTexture2D(gpuPool)", hr);
        return false;
    }
    impl_->gpuPoolW = width;
    impl_->gpuPoolH = height;
    return true;
}

DxgiCapturer::Result DxgiCapturer::CaptureFrameGpu(int timeoutMs, GpuFrame* out,
                                                    std::string* errorOut) {
    if (!impl_->duplication) {
        if (!impl_->device) {
            if (errorOut) *errorOut = "capturer not initialised";
            return Result::kError;
        }
        if (!createDuplication(errorOut)) return Result::kError;
    }

    if (impl_->frameHeld) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = impl_->duplication->AcquireNextFrame(
        static_cast<UINT>(timeoutMs), &info, res.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::kTimeout;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        impl_->duplication.Reset();
        if (errorOut) *errorOut = "DXGI access lost; reinit required";
        return Result::kError;
    }
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("AcquireNextFrame", hr);
        return Result::kError;
    }
    impl_->frameHeld = true;

    // Cursor shape query, same as the CPU path.
    if (info.PointerShapeBufferSize > 0) {
        if (impl_->shapeBuf.size() < info.PointerShapeBufferSize) {
            impl_->shapeBuf.resize(info.PointerShapeBufferSize);
        }
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
        UINT used = 0;
        HRESULT hr2 = impl_->duplication->GetFramePointerShape(
            info.PointerShapeBufferSize,
            impl_->shapeBuf.data(),
            &used,
            &shapeInfo);
        if (SUCCEEDED(hr2) && used > 0) {
            decodeCursorShape(shapeInfo, impl_->shapeBuf.data(), used,
                              &impl_->pendingCursor);
            if (!impl_->pendingCursor.bgra.empty()) {
                impl_->hasPendingCursor = true;
            }
        }
    }

    ComPtr<ID3D11Texture2D> captured;
    hr = res.As(&captured);
    if (FAILED(hr)) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        if (errorOut) *errorOut = hresultMsg("QI ID3D11Texture2D", hr);
        return Result::kError;
    }
    D3D11_TEXTURE2D_DESC capDesc;
    captured->GetDesc(&capDesc);

    if (!ensureGpuPool(static_cast<int>(capDesc.Width),
                       static_cast<int>(capDesc.Height), errorOut)) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        return Result::kError;
    }

    // GPU-to-GPU copy from DXGI-owned texture into our pool. Fast (~sub-
    // ms at 1440p) and keeps the DXGI resource untouched so ReleaseFrame
    // can succeed on the next Acquire.
    impl_->context->CopyResource(impl_->gpuPool.Get(), captured.Get());

    out->width = static_cast<int>(capDesc.Width);
    out->height = static_cast<int>(capDesc.Height);
    out->gpuTexture = impl_->gpuPool.Get();
    out->timestamp = std::chrono::system_clock::now();

    // DXGI wants the frame released BEFORE the next Acquire. We've already
    // copied into our pool, so we can let go immediately.
    impl_->duplication->ReleaseFrame();
    impl_->frameHeld = false;
    return Result::kFrame;
}
