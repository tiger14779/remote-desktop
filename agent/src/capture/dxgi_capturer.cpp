#include "capture/dxgi_capturer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {
std::string hresultMsg(const char* what, HRESULT hr) {
    std::stringstream ss;
    ss << what << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}
} // namespace

struct DxgiCapturer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_OUTPUT_DESC outputDesc{};
    bool frameHeld = false; // true between AcquireNextFrame and ReleaseFrame
};

DxgiCapturer::DxgiCapturer() : impl_(std::make_unique<Impl>()) {}

DxgiCapturer::~DxgiCapturer() {
    if (impl_->frameHeld && impl_->duplication) {
        impl_->duplication->ReleaseFrame();
    }
}

bool DxgiCapturer::Init(std::string* errorOut) {
    if (!createDevice(errorOut)) return false;
    if (!createDuplication(errorOut)) return false;
    return true;
}

bool DxgiCapturer::createDevice(std::string* errorOut) {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        impl_->device.GetAddressOf(), &got,
        impl_->context.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("D3D11CreateDevice", hr);
        return false;
    }
    return true;
}

bool DxgiCapturer::createDuplication(std::string* errorOut) {
    ComPtr<IDXGIDevice> dxgiDev;
    HRESULT hr = impl_->device.As(&dxgiDev);
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("QI IDXGIDevice", hr);
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDev->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("GetAdapter", hr);
        return false;
    }
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, output.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("EnumOutputs(0)", hr);
        return false;
    }
    output->GetDesc(&impl_->outputDesc);
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("QI IDXGIOutput1", hr);
        return false;
    }
    hr = output1->DuplicateOutput(impl_->device.Get(), impl_->duplication.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hresultMsg("DuplicateOutput", hr);
        return false;
    }
    return true;
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
        if (errorOut) *errorOut = "capturer not initialised";
        return Result::kError;
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

    // If only the mouse moved (no desktop pixels changed), DXGI still returns
    // a frame but LastPresentTime is 0. Treat that as "no useful frame" so the
    // caller can poll again and we don't burn bandwidth on cursor jiggles.
    if (info.LastPresentTime.QuadPart == 0) {
        impl_->duplication->ReleaseFrame();
        impl_->frameHeld = false;
        return Result::kTimeout;
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
