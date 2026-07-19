#include "net/net_prefs.h"

#include <atomic>
#include <cstdlib>

namespace net_prefs {
namespace {

std::atomic<bool>& state() {
    // Seeded once from RD_DISABLE_UDP=1 (the historic kill-switch); after
    // that the GUI toggle owns the value. Function-local static so the seed
    // runs on first use, not before main() where getenv is fine but the
    // ordering is easier to reason about here.
    static std::atomic<bool> s = [] {
        const char* env = std::getenv("RD_DISABLE_UDP");
        return env && env[0] == '1';
    }();
    return s;
}

} // namespace

bool ForceTcpVideo() { return state().load(std::memory_order_acquire); }

void SetForceTcpVideo(bool on) {
    state().store(on, std::memory_order_release);
}

} // namespace net_prefs
