#pragma once

// LocalIPv4Addresses: enumerate every non-loopback, non-link-local IPv4
// address currently bound to an operational adapter on this machine.
//
// Used for LAN candidate exchange: if two peers happen to be behind the same
// router (or share a corporate VPN segment), one of these IPs will be
// directly reachable and hole-punching succeeds trivially — faster than
// even the best public P2P path. Windows-only (uses iphlpapi).

#include <string>
#include <vector>

namespace udp_lan {

std::vector<std::string> LocalIPv4Addresses();

} // namespace udp_lan
