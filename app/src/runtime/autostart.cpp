#include "runtime/autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <windows.h>

namespace Autostart {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

QString currentExePath() {
    // GetModuleFileNameW resolves the running exe regardless of cwd, symlinks,
    // or how it was launched — this is what we want persisted in Run so a
    // move of the install folder is detected via re-tick of the checkbox.
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        // Fall back to Qt's applicationFilePath — same value under normal
        // conditions but at least gives us a non-empty string if the WinAPI
        // call somehow failed.
        return QCoreApplication::applicationFilePath();
    }
    return QString::fromWCharArray(buf, int(n));
}

std::wstring quotedExePath() {
    // Windows registry Run entries containing a path with spaces MUST be
    // double-quoted, otherwise the shell parses the first space as an
    // argument boundary and tries to launch "C:\Program.exe" with args
    // "Files\..." — the classic "Program.exe not found" auto-start bug.
    QString path = currentExePath();
    // Normalise separators so the value looks like everyone else's Run
    // entries (backslashes). Qt happily works with either but Windows tools
    // (regedit, msconfig) show forward slashes as weird.
    path = QDir::toNativeSeparators(path);
    std::wstring w;
    w.reserve(path.size() + 2);
    w.push_back(L'"');
    w.append(reinterpret_cast<const wchar_t*>(path.utf16()));
    w.push_back(L'"');
    return w;
}

std::wstring valueName() {
    // ASCII app name is fine; embed as wide via mbstowcs-avoiding cast.
    std::wstring w;
    for (const char* p = kAppName; *p; ++p) w.push_back(wchar_t(*p));
    return w;
}

} // namespace

bool isEnabled() {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey)
        != ERROR_SUCCESS) {
        return false;
    }
    // Query with a zero-length buffer first to get the required size, then
    // verify the type is REG_SZ. Anything else (e.g. REG_EXPAND_SZ someone
    // hand-edited in) we treat as "not our value" and report disabled so
    // the UI stays consistent.
    DWORD type = 0;
    DWORD sizeBytes = 0;
    const auto vn = valueName();
    LSTATUS s = ::RegQueryValueExW(hKey, vn.c_str(), nullptr, &type, nullptr, &sizeBytes);
    ::RegCloseKey(hKey);
    return s == ERROR_SUCCESS && type == REG_SZ && sizeBytes > sizeof(wchar_t);
}

bool enable() {
    HKEY hKey = nullptr;
    // KEY_SET_VALUE alone is enough; no need to CreateKey because Run is
    // always present. Using OpenKey (not Create) makes the intent obvious.
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey)
        != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring val = quotedExePath();
    const std::wstring vn = valueName();
    // +1 to include the terminating NUL — RegSetValueEx wants the byte count
    // with the NUL for REG_SZ values.
    const DWORD bytes = DWORD((val.size() + 1) * sizeof(wchar_t));
    const LSTATUS s = ::RegSetValueExW(
        hKey, vn.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(val.c_str()), bytes);
    ::RegCloseKey(hKey);
    return s == ERROR_SUCCESS;
}

bool disable() {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey)
        != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring vn = valueName();
    const LSTATUS s = ::RegDeleteValueW(hKey, vn.c_str());
    ::RegCloseKey(hKey);
    // ERROR_FILE_NOT_FOUND is fine — the value was already gone, which is
    // the state the caller wanted.
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

} // namespace Autostart
