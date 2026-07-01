#include "udp/lan_ips.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace udp_lan {

std::vector<std::string> LocalIPv4Addresses() {
    std::vector<std::string> out;

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = 15 * 1024;  // starting guess; grows on ERROR_BUFFER_OVERFLOW
    std::vector<std::uint8_t> buf(size);

    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        DWORD rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(size);
            continue;
        }
        if (rc != NO_ERROR) return out;

        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            // Skip virtual-only adapters where possible. Loopback stays
            // filtered by IfType; VMware / WSL show up as regular ETHERNET
            // so we can't drop them without hostile heuristics, which
            // would just add fragile string matches. Their addresses are
            // harmless to probe — they just won't reach anyone.
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* sa4 = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                char ipbuf[INET_ADDRSTRLEN] = {0};
                if (!inet_ntop(AF_INET, &sa4->sin_addr, ipbuf, sizeof(ipbuf))) continue;

                // 169.254.x.x — automatic private addressing, means DHCP
                // failed, unreachable from anyone else. Drop.
                if (std::strncmp(ipbuf, "169.254.", 8) == 0) continue;
                // 127.x — loopback, already filtered by IfType but belt-and-
                // braces for weirdly-configured hosts.
                if (std::strncmp(ipbuf, "127.", 4) == 0) continue;

                out.emplace_back(ipbuf);
            }
        }
        break;
    }
    return out;
}

} // namespace udp_lan
