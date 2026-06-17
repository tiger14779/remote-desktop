#include "input/input_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace input {

namespace {

// Message type constants — kept here as raw bytes to avoid a dep on net/frame.h
// from the input library (input doesn't need to talk to the network).
constexpr std::uint8_t kMouseMove   = 0x20;
constexpr std::uint8_t kMouseButton = 0x21;
constexpr std::uint8_t kKeyEvent    = 0x22;

std::uint16_t readBE16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}

bool injectMouseMove(std::uint16_t nx, std::uint16_t ny, std::string* err) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = nx;
    in.mi.dy = ny;
    // MOUSEEVENTF_ABSOLUTE without MOUSEEVENTF_VIRTUALDESK maps 0..65535 to the
    // primary monitor only. Good enough for single-display targets; revisit
    // when we add multi-monitor support.
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    if (SendInput(1, &in, sizeof(in)) != 1) {
        if (err) *err = "SendInput(mouse_move) rejected";
        return false;
    }
    return true;
}

bool injectMouseButton(std::uint8_t button, bool down, std::string* err) {
    DWORD flags = 0;
    DWORD mouseData = 0;
    switch (button) {
        case 0: flags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
        case 1: flags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
        case 2: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 3: flags = down ? MOUSEEVENTF_XDOWN      : MOUSEEVENTF_XUP;
                mouseData = XBUTTON1; break;
        case 4: flags = down ? MOUSEEVENTF_XDOWN      : MOUSEEVENTF_XUP;
                mouseData = XBUTTON2; break;
        default:
            if (err) *err = "unknown mouse button id";
            return false;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = mouseData;
    if (SendInput(1, &in, sizeof(in)) != 1) {
        if (err) *err = "SendInput(mouse_button) rejected";
        return false;
    }
    return true;
}

// Keys that physically live on the "extended" side of the keyboard and must
// carry KEYEVENTF_EXTENDEDKEY for Windows to deliver them as the user expects
// (e.g. without it, right-Ctrl shows up as left-Ctrl in games / IMEs).
bool isExtendedKey(std::uint16_t vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL:
        case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
            return true;
        default:
            return false;
    }
}

bool injectKey(std::uint16_t vk, bool down, std::string* err) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    DWORD flags = down ? 0 : KEYEVENTF_KEYUP;
    if (isExtendedKey(vk)) flags |= KEYEVENTF_EXTENDEDKEY;
    in.ki.dwFlags = flags;
    if (SendInput(1, &in, sizeof(in)) != 1) {
        if (err) *err = "SendInput(key) rejected";
        return false;
    }
    return true;
}

} // namespace

bool HandleInputFrame(std::uint8_t msgType,
                      const std::uint8_t* data, std::size_t size,
                      std::string* errorOut) {
    switch (msgType) {
        case kMouseMove:
            if (size != 4) {
                if (errorOut) *errorOut = "MOUSE_MOVE payload must be 4 bytes";
                return false;
            }
            return injectMouseMove(readBE16(data), readBE16(data + 2), errorOut);
        case kMouseButton:
            if (size != 2) {
                if (errorOut) *errorOut = "MOUSE_BUTTON payload must be 2 bytes";
                return false;
            }
            return injectMouseButton(data[0], data[1] != 0, errorOut);
        case kKeyEvent:
            if (size != 4) {
                if (errorOut) *errorOut = "KEY_EVENT payload must be 4 bytes";
                return false;
            }
            return injectKey(readBE16(data), data[2] != 0, errorOut);
        default:
            if (errorOut) *errorOut = "unknown input frame type";
            return false;
    }
}

} // namespace input
