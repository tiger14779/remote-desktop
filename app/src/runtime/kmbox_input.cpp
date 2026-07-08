#include "runtime/kmbox_input.h"

#include "runtime/kmbox_net.h"

// input_injector lives in agent_core; we only need the two API entries
// (HandleInputFrame + SetInputRouter). The header path is set up by
// app/CMakeLists.txt.
#include "input/input_injector.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ShellScalingApi.h>

#include <algorithm>

#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>

namespace {

// Message type constants (mirror shared/net/frame.h — inlined to keep this
// module dep-free from the network frame layer).
constexpr std::uint8_t kMouseMove   = 0x20;
constexpr std::uint8_t kMouseButton = 0x21;
constexpr std::uint8_t kKeyEvent    = 0x22;
constexpr std::uint8_t kMouseWheel  = 0x23;

std::uint16_t readBE16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}

// Primary-monitor DPI scale factor (96 DPI = 1.0, 144 DPI = 1.5, ...).
// Cached once per process, because a real DPI change means the desktop
// resolution changed too and the whole session should reconnect anyway.
// Uses GetDpiForMonitor over MONITOR_FROM_WINDOW(HWND_DESKTOP) rather than
// process-wide GetDeviceCaps so it gives the correct value on a per-monitor
// DPI-aware process (which we are — SetProcessDpiAwarenessContext V2 in
// main.cpp).
double dpiScale() {
    static double s_scale = 0.0;
    if (s_scale > 0.0) return s_scale;
    HMONITOR mon = ::MonitorFromWindow(::GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    (void)::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    s_scale = dpiX > 0 ? dpiX / 96.0 : 1.0;
    std::cerr << "[kmbox] primary monitor DPI = " << dpiX
              << " -> scale factor = " << s_scale << "\n";
    // Informational: report whether EPP is on. Not asking the user to
    // change it — the mouse-move path is designed to survive both states
    // via proportional damping (see routeFrame's kMouseMove branch).
    int mouseParams[3] = {0, 0, 0};
    if (::SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams, 0)) {
        std::cerr << "[kmbox] Windows 'Enhance Pointer Precision' is "
                  << (mouseParams[2] != 0 ? "ON" : "OFF")
                  << " (damping compensates automatically)\n";
    }
    return s_scale;
}

// Windows VK_* → USB HID Usage ID. Table is intentionally big-ish so common
// remote-desktop input (typing, arrows, function keys, media navigation)
// stays fully hardware-routed. Missing keys → we drop with a stderr warning.
const std::unordered_map<std::uint16_t, int>& vkToHid() {
    static const std::unordered_map<std::uint16_t, int> m = {
        // Modifiers
        {VK_CONTROL,  kmbox::hid::KEY_LEFTCONTROL},
        {VK_LCONTROL, kmbox::hid::KEY_LEFTCONTROL},
        {VK_RCONTROL, kmbox::hid::KEY_RIGHTCONTROL},
        {VK_SHIFT,    kmbox::hid::KEY_LEFTSHIFT},
        {VK_LSHIFT,   kmbox::hid::KEY_LEFTSHIFT},
        {VK_RSHIFT,   kmbox::hid::KEY_RIGHTSHIFT},
        {VK_MENU,     kmbox::hid::KEY_LEFTALT},
        {VK_LMENU,    kmbox::hid::KEY_LEFTALT},
        {VK_RMENU,    kmbox::hid::KEY_RIGHTALT},
        {VK_LWIN,     kmbox::hid::KEY_LEFT_GUI},
        {VK_RWIN,     kmbox::hid::KEY_RIGHT_GUI},

        // Navigation / editing
        {VK_RETURN,   kmbox::hid::KEY_ENTER},
        {VK_ESCAPE,   kmbox::hid::KEY_ESCAPE},
        {VK_BACK,     kmbox::hid::KEY_BACKSPACE},
        {VK_TAB,      kmbox::hid::KEY_TAB},
        {VK_SPACE,    kmbox::hid::KEY_SPACE},
        {VK_CAPITAL,  kmbox::hid::KEY_CAPSLOCK},
        {VK_LEFT,     kmbox::hid::KEY_LEFT},
        {VK_RIGHT,    kmbox::hid::KEY_RIGHT},
        {VK_UP,       kmbox::hid::KEY_UP},
        {VK_DOWN,     kmbox::hid::KEY_DOWN},
        {VK_HOME,     kmbox::hid::KEY_HOME},
        {VK_END,      kmbox::hid::KEY_END},
        {VK_PRIOR,    kmbox::hid::KEY_PAGEUP},
        {VK_NEXT,     kmbox::hid::KEY_PAGEDOWN},
        {VK_INSERT,   kmbox::hid::KEY_INSERT},
        {VK_DELETE,   kmbox::hid::KEY_DELETE},
        {VK_SNAPSHOT, kmbox::hid::KEY_PRINTSCREEN},
        {VK_SCROLL,   kmbox::hid::KEY_SCROLLLOCK},
        {VK_PAUSE,    kmbox::hid::KEY_PAUSE},

        // Punctuation (ANSI layout)
        {VK_OEM_MINUS,  kmbox::hid::KEY_MINUS},
        {VK_OEM_PLUS,   kmbox::hid::KEY_EQUAL},   // '=' shares physical key with '+'
        {VK_OEM_4,      kmbox::hid::KEY_LBRACKET},
        {VK_OEM_6,      kmbox::hid::KEY_RBRACKET},
        {VK_OEM_5,      kmbox::hid::KEY_BACKSLASH},
        {VK_OEM_1,      kmbox::hid::KEY_SEMICOLON},
        {VK_OEM_7,      kmbox::hid::KEY_APOSTROPHE},
        {VK_OEM_3,      kmbox::hid::KEY_GRAVE},
        {VK_OEM_COMMA,  kmbox::hid::KEY_COMMA},
        {VK_OEM_PERIOD, kmbox::hid::KEY_PERIOD},
        {VK_OEM_2,      kmbox::hid::KEY_SLASH},

        // Numpad
        {VK_NUMLOCK,  kmbox::hid::KEY_NUMLOCK},
        {VK_DIVIDE,   kmbox::hid::KEY_KP_DIVIDE},
        {VK_MULTIPLY, kmbox::hid::KEY_KP_MULTIPLY},
        {VK_SUBTRACT, kmbox::hid::KEY_KP_MINUS},
        {VK_ADD,      kmbox::hid::KEY_KP_PLUS},
        {VK_DECIMAL,  kmbox::hid::KEY_KP_DOT},
    };
    return m;
}

// A-Z, 0-9, F1-F12, numpad 0-9 fill in via arithmetic. Keeps the static map
// smaller and matches the way both VK_ and HID Usage IDs allocate contiguous
// ranges.
int mapVkToHid(std::uint16_t vk) {
    // Letters: VK_A..VK_Z is 0x41..0x5A; HID KEY_A..KEY_Z is 0x04..0x1D.
    if (vk >= 'A' && vk <= 'Z') return kmbox::hid::KEY_A + (vk - 'A');
    // Top-row digits: VK_0..VK_9 is 0x30..0x39; HID KEY_1..KEY_0 is 0x1E..0x27
    // — note '1' is HID 0x1E while '0' is HID 0x27, so we branch.
    if (vk == '0') return kmbox::hid::KEY_0;
    if (vk >= '1' && vk <= '9') return kmbox::hid::KEY_1 + (vk - '1');
    // F1..F12: VK_F1..VK_F12 is 0x70..0x7B; HID KEY_F1..KEY_F12 is 0x3A..0x45.
    if (vk >= VK_F1 && vk <= VK_F12) return kmbox::hid::KEY_F1 + (vk - VK_F1);
    // Numpad digits: VK_NUMPAD0..9 is 0x60..0x69; HID KEY_KP_1..KP_0.
    if (vk == VK_NUMPAD0) return kmbox::hid::KEY_KP_0;
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
        return kmbox::hid::KEY_KP_1 + (vk - VK_NUMPAD1);

    const auto& m = vkToHid();
    auto it = m.find(vk);
    return it == m.end() ? 0 : it->second;
}

} // namespace

KmboxInputRouter::KmboxInputRouter(QObject* parent) : QObject(parent) {}

KmboxInputRouter::~KmboxInputRouter() {
    disable();  // stops heartbeat + tears down box_
}

bool KmboxInputRouter::enable(const QString& ip, quint16 port,
                              const QString& uuidHex, QString* err) {
    // Make sure any previous heartbeat is torn down before we spin a new
    // one — enable() may be called to reconnect after a config edit.
    {
        heartbeatStop_.store(true);
        heartbeatCv_.notify_all();
    }
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
    heartbeatStop_.store(false);

    std::lock_guard<std::mutex> lk(mu_);
    // Recreate on every enable — the SDK's close() zeroes internal state, so
    // a fresh box_ is the simplest way to ensure a clean CONNECT indexpts.
    auto fresh = std::make_unique<kmbox::KmBoxNet>();
    if (!fresh->init(ip, port, uuidHex, err)) {
        return false;
    }
    box_ = std::move(fresh);
    enabled_.store(true);
    boxAlive_.store(true);

    // Install ourselves as the process-wide input router. Captured `this`
    // pointer is safe because disable() (and the destructor) unregister
    // the router before releasing box_.
    input::SetInputRouter([this](std::uint8_t t, const std::uint8_t* d,
                                  std::size_t s) {
        return routeFrame(t, d, s);
    });
    emit statusChanged(QStringLiteral("kmbox 已连接: %1:%2").arg(ip).arg(port));

    // Start the 3s heartbeat. Runs until disable() flips heartbeatStop_.
    heartbeatThread_ = std::thread([this]() {
        constexpr int kIntervalMs = 3000;
        constexpr int kFailuresBeforeDead = 2;  // ~6s to declare dead
        int consecFailures = 0;
        while (!heartbeatStop_.load()) {
            // Interruptible sleep so disable() doesn't have to wait a full
            // 3s to join.
            {
                std::unique_lock<std::mutex> lk(heartbeatMu_);
                heartbeatCv_.wait_for(lk, std::chrono::milliseconds(kIntervalMs),
                                       [this]() { return heartbeatStop_.load(); });
            }
            if (heartbeatStop_.load()) break;

            // Take a stable snapshot of the box pointer under mu_ so a
            // concurrent disable() can't yank it mid-ping. The ping call
            // itself is thread-safe (KmBoxNet::sendCmd holds its own mutex),
            // but the unique_ptr could be nulled.
            QString err;
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (box_) ok = box_->ping(&err);
            }
            if (ok) {
                consecFailures = 0;
                if (!boxAlive_.exchange(true)) {
                    emit boxAliveChanged(true);
                    std::cerr << "[kmbox] heartbeat: box back online\n";
                }
            } else {
                ++consecFailures;
                std::cerr << "[kmbox] heartbeat #" << consecFailures
                          << " failed: " << err.toStdString() << "\n";
                if (consecFailures >= kFailuresBeforeDead) {
                    if (boxAlive_.exchange(false)) {
                        emit boxAliveChanged(false);
                        std::cerr << "[kmbox] heartbeat: box declared DEAD, "
                                     "input routing bypassed to SendInput\n";
                    }
                }
            }
        }
    });
    return true;
}

void KmboxInputRouter::disable() {
    // Deregister BEFORE tearing down box_ — otherwise a frame arriving on
    // the network thread mid-teardown would dereference a dying object.
    input::SetInputRouter(nullptr);

    // Stop the heartbeat first so it can't spuriously flip boxAlive_ back
    // to true while we're shutting down.
    heartbeatStop_.store(true);
    heartbeatCv_.notify_all();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();

    std::lock_guard<std::mutex> lk(mu_);
    enabled_.store(false);
    boxAlive_.store(false);
    if (box_) {
        box_->close();
        box_.reset();
    }
    emit statusChanged(QStringLiteral("kmbox 已关闭"));
}

bool KmboxInputRouter::routeFrame(std::uint8_t msgType, const std::uint8_t* data,
                                  std::size_t size) {
    if (!enabled_.load()) return false;
    // Heartbeat says box is dead — decline this frame so HandleInputFrame
    // falls back to SendInput. The user is temporarily unsafe (WeChat Work
    // will see the LLKHF_INJECTED flag) but at least their input works;
    // the banner shows a red warning so they can react.
    if (!boxAlive_.load()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!box_ || !box_->isConnected()) return false;

    if (msgType == kMouseMove) {
        if (size != 4) return false;
        // Why not mouseMoveAuto: the box's automove firmware processes each
        // command over its ms duration. At 60fps input × ~12ms each, the
        // per-second automove workload was ~720ms — commands piled up in
        // the box's queue and cursor motion visibly lagged by 1-2 seconds
        // once the stream got going. Straight mouseMoveRel has no such
        // queue.
        //
        // Why not naive mouseMoveRel with the raw delta: Windows'
        // 'Enhance Pointer Precision' (default: ON, and users hate being
        // asked to disable it) applies a non-linear amplification curve —
        // dx=800 in one HID report becomes ~2000 physical pixels of
        // actual movement. Overshoot → next frame's delta is bigger the
        // other way → runaway oscillation ("乱飞").
        //
        // The compromise: mouseMoveRel with a proportional-damping factor.
        // Send `k * delta` per frame, where 0 < k < 1. Under EPP amplifi-
        // cation factor `a`, the effective per-frame movement is `k*a`
        // times delta. The math:
        //   * a=1 (EPP off, small delta):  k*1 = 0.5 → converges in ~5
        //     frames (~80ms at 60fps) monotonically.
        //   * a=2 (EPP on, moderate delta): k*2 = 1.0 → target hit in 1
        //     frame.
        //   * a=3 (EPP on, large delta):    k*3 = 1.5 → overshoot 50%,
        //     then next frame's damped correction shrinks it, 3 frames
        //     to <5% error.
        // For k=0.5, |1 - k*a| < 1 for any a in (0, 4), which covers the
        // full realistic EPP range → always convergent, no runaway.
        const std::uint16_t nx = readBE16(data);
        const std::uint16_t ny = readBE16(data + 2);
        const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
        const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
        const int targetX = static_cast<int>(
            (static_cast<long long>(nx) * screenW) / 65535);
        const int targetY = static_cast<int>(
            (static_cast<long long>(ny) * screenH) / 65535);
        POINT cur{};
        ::GetCursorPos(&cur);
        const int physDx = targetX - cur.x;
        const int physDy = targetY - cur.y;
        // Sub-pixel dead zone.
        if (std::abs(physDx) <= 1 && std::abs(physDy) <= 1) return true;

        const double s = dpiScale();
        // Adaptive damping matched to Windows' EPP amplification curve.
        // The curve peaks around 50-150-pixel deltas (2.5-3.5× amp), then
        // saturates. So we damp HARDEST in that middle band. Empirical
        // values chosen so k*amp ≈ 1 across all bands:
        //
        //   band                phys   k     EPP amp   effective  overshoot
        //   fine control        ≤10    1.0   ~1.0×     1.0×       none
        //   settling            ≤30    0.55  ~1.7×     0.94×      slight undershoot
        //   LIGHT SHAKE ZONE    ≤80    0.30  ~2.8×     0.84×      undershoot
        //   fast move           ≤200   0.28  ~3.2×     0.90×      undershoot
        //   big drag saturated  >200   0.42  ~3.5×     1.47×      moderate overshoot
        //
        // Deliberately biased toward undershoot in the mid range — that's
        // where a "light quick shake" lives, and even a small overshoot
        // there becomes visible flying. Big drags accept some overshoot
        // because the correction lands so far the human doesn't notice.
        const int magPhys = std::max(std::abs(physDx), std::abs(physDy));
        double k;
        if      (magPhys <= 10)   k = 1.0;
        else if (magPhys <= 30)   k = 0.55;
        else if (magPhys <= 80)   k = 0.30;
        else if (magPhys <= 200)  k = 0.28;
        else                       k = 0.42;

        int hidDx = static_cast<int>(std::lround((physDx * k) / s));
        int hidDy = static_cast<int>(std::lround((physDy * k) / s));
        // ±1 floor so a small residual isn't rounded away forever.
        if (hidDx == 0 && physDx != 0) hidDx = (physDx > 0) ? 1 : -1;
        if (hidDy == 0 && physDy != 0) hidDy = (physDy > 0) ? 1 : -1;
        // Hard cap on per-frame HID delta. Safety net for the "unknown
        // extreme EPP" case — even at max amp 4×, a HID delta of 50 yields
        // only 200 physical pixels of movement per frame, no matter how
        // pathological the user's mouse settings. Big drags still cross
        // a 2560-wide screen in ~12 frames (200ms) which is fast enough.
        constexpr int kMaxHidPerFrame = 50;
        if (hidDx >  kMaxHidPerFrame) hidDx =  kMaxHidPerFrame;
        if (hidDx < -kMaxHidPerFrame) hidDx = -kMaxHidPerFrame;
        if (hidDy >  kMaxHidPerFrame) hidDy =  kMaxHidPerFrame;
        if (hidDy < -kMaxHidPerFrame) hidDy = -kMaxHidPerFrame;
        return box_->mouseMoveRel(short(hidDx), short(hidDy));
    }
    if (msgType == kMouseButton) {
        if (size != 2) return false;
        const std::uint8_t btn = data[0];
        const bool down = data[1] != 0;
        switch (btn) {
            case 0: return box_->mouseLeft(down);
            case 1: return box_->mouseRight(down);
            case 2: return box_->mouseMiddle(down);
            default:
                // X1 / X2 side buttons — KMBox HID report has no dedicated
                // slots for these. Rather than silently drop, decline and
                // let SendInject handle it. Trade-off: X1/X2 will get the
                // LLKHF_INJECTED flag but the common buttons stay hardware.
                return false;
        }
    }
    if (msgType == kKeyEvent) {
        if (size != 4) return false;
        const std::uint16_t vk = readBE16(data);
        const bool down = data[2] != 0;
        const int hidKey = mapVkToHid(vk);
        if (hidKey == 0) {
            std::cerr << "[kmbox] VK 0x" << std::hex << vk << std::dec
                      << " has no HID mapping — dropped\n";
            // Return true so we DON'T fall back to SendInput — the whole
            // point of enabling KMBox mode is that SendInput must never
            // fire for this session.
            return true;
        }
        return down ? box_->keyDown(hidKey) : box_->keyUp(hidKey);
    }
    if (msgType == kMouseWheel) {
        if (size != 2) return false;
        // Viewer sends Windows WHEEL_DELTA units (120 per notch, signed).
        // KMBox's mouseWheel packet takes an int wheel value — real USB HID
        // mice send +1 / -1 per detent in the standard HID wheel usage,
        // and KMBox's SDK forwards that raw. Divide by WHEEL_DELTA (120)
        // to convert; keep at least ±1 magnitude for a non-zero input so a
        // small trackpad-style scroll still produces a visible tick.
        const std::int16_t deltaWD = static_cast<std::int16_t>(readBE16(data));
        if (deltaWD == 0) return true;
        int wheelTicks = deltaWD / 120;
        if (wheelTicks == 0) wheelTicks = (deltaWD > 0) ? 1 : -1;
        return box_->mouseWheel(wheelTicks);
    }
    return false;
}
