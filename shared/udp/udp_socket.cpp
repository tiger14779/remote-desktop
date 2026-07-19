#include "udp/udp_socket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace {

// Mirror TcpClient's WSAStartup refcount so a process that uses both pays
// for init exactly once. Declared inline here (not shared with TcpClient)
// because the relay client and udp socket can be linked into independent
// translation units; each gets its own refcount, both safely bracket
// WSAStartup/WSACleanup over their lifetime.
std::mutex& wsaMu() { static std::mutex m; return m; }
int& wsaCount() { static int c = 0; return c; }

constexpr std::uint64_t kInvalid = static_cast<std::uint64_t>(INVALID_SOCKET);

std::string wsaErr(const char* what, int err) {
    std::stringstream ss;
    ss << what << " failed: WSAError " << err;
    return ss.str();
}

// Fill sin_addr from either a numeric IPv4 string ("38.55.192.55") OR a
// hostname ("yuancheng.skopn.co"). inet_pton alone only handles the first;
// we hit "inet_pton failed for <domain>" the moment the relay reported a
// domain-name UDP endpoint. getaddrinfo covers both cases, and its cache
// (Windows resolver) means the per-call overhead on a hot-loop sendto is
// microseconds after the first lookup.
//
// Returns true on success. On failure, err is populated with a human-
// readable reason and sin_addr is left untouched.
bool resolveIPv4(const std::string& hostOrIp, in_addr* out,
                 std::string* err) {
    if (hostOrIp.empty()) {
        if (err) *err = "empty host";
        return false;
    }
    // Fast path — already a numeric IP.
    if (inet_pton(AF_INET, hostOrIp.c_str(), out) == 1) return true;

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(hostOrIp.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        if (err) *err = "getaddrinfo failed for " + hostOrIp +
                         " (rc=" + std::to_string(rc) + ")";
        if (res) freeaddrinfo(res);
        return false;
    }
    *out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

} // namespace

UdpSocket::UdpSocket() : sock_(kInvalid) {
    std::lock_guard<std::mutex> g(wsaMu());
    if (wsaCount()++ == 0) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}

UdpSocket::~UdpSocket() {
    Close();
    std::lock_guard<std::mutex> g(wsaMu());
    if (--wsaCount() == 0) {
        WSACleanup();
    }
}

bool UdpSocket::Bind(const std::string& localHost, int localPort,
                     std::string* errorOut) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (errorOut) *errorOut = wsaErr("socket", WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(localPort));
    if (localHost.empty() || localHost == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        std::string rerr;
        if (!resolveIPv4(localHost, &addr.sin_addr, &rerr)) {
            if (errorOut) *errorOut = rerr;
            closesocket(s);
            return false;
        }
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errorOut) *errorOut = wsaErr("bind", WSAGetLastError());
        closesocket(s);
        return false;
    }

    // Discover the actually-bound port (we may have asked for 0 = OS-picked).
    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        localPort_ = ntohs(bound.sin_port);
    } else {
        localPort_ = localPort;
    }

    // Big-ish receive buffer so a burst of incoming packets while the
    // reader is blocked in encoding/decoding doesn't drop datagrams.
    // 4 MiB matches what RustDesk uses and is generous for screen-streaming
    // bursts without becoming a latency reservoir (UDP receive buffer
    // doesn't pace the sender like TCP does).
    int rcvBuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    sock_ = static_cast<std::uint64_t>(s);
    return true;
}

bool UdpSocket::SendTo(const Endpoint& dst,
                       const std::uint8_t* data, std::size_t size,
                       std::string* errorOut) {
    if (sock_ == kInvalid) {
        if (errorOut) *errorOut = "socket not open";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(dst.port));
    std::string rerr;
    if (!resolveIPv4(dst.host, &addr.sin_addr, &rerr)) {
        if (errorOut) *errorOut = rerr;
        return false;
    }
    const int sent = sendto(static_cast<SOCKET>(sock_),
                            reinterpret_cast<const char*>(data),
                            static_cast<int>(size), 0,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (sent == SOCKET_ERROR) {
        if (errorOut) *errorOut = wsaErr("sendto", WSAGetLastError());
        return false;
    }
    return true;
}

int UdpSocket::RecvFrom(std::uint8_t* buf, std::size_t bufSize,
                        Endpoint* src,
                        int timeoutMs,
                        std::string* errorOut) {
    if (sock_ == kInvalid) {
        if (errorOut) *errorOut = "socket not open";
        return -1;
    }
    // Set recv timeout per-call so different callers can use different
    // budgets without stepping on each other (only one thread should be
    // calling RecvFrom at a time, per the class contract).
    DWORD t = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&t), sizeof(t));

    sockaddr_in addr{};
    int addrLen = sizeof(addr);
    const int got = recvfrom(static_cast<SOCKET>(sock_),
                             reinterpret_cast<char*>(buf),
                             static_cast<int>(bufSize), 0,
                             reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (got == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) {
            // Timeout — caller can retry. Distinguish from a hard error by
            // leaving errorOut empty.
            return 0;
        }
        if (errorOut) *errorOut = wsaErr("recvfrom", e);
        return -1;
    }
    if (src) {
        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
        src->host = ipbuf;
        src->port = ntohs(addr.sin_port);
    }
    return got;
}

void UdpSocket::Close() {
    if (sock_ != kInvalid) {
        // shutdown before closesocket for the same reason as TcpClient::Close:
        // some LSPs leave a blocked recvfrom parked when the socket is closed
        // out from under it. shutdown gives us a documented wakeup path.
        const SOCKET s = static_cast<SOCKET>(sock_);
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
        sock_ = kInvalid;
    }
}
