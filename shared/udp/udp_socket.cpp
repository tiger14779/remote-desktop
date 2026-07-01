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
        if (inet_pton(AF_INET, localHost.c_str(), &addr.sin_addr) != 1) {
            if (errorOut) *errorOut = "inet_pton failed for " + localHost;
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
    if (inet_pton(AF_INET, dst.host.c_str(), &addr.sin_addr) != 1) {
        if (errorOut) *errorOut = "inet_pton failed for " + dst.host;
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
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = kInvalid;
    }
}
