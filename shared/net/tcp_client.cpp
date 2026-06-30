#include "net/tcp_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace {
std::string wsaErr(const char* what, int err) {
    std::stringstream ss;
    ss << what << " failed: WSAError " << err;
    return ss.str();
}
constexpr std::uint64_t kInvalid = static_cast<std::uint64_t>(INVALID_SOCKET);
} // namespace

int TcpClient::instanceCount_ = 0;
std::mutex TcpClient::globalMu_{};

TcpClient::TcpClient() : sock_(kInvalid) {
    std::lock_guard<std::mutex> g(globalMu_);
    if (instanceCount_++ == 0) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}

TcpClient::~TcpClient() {
    Close();
    std::lock_guard<std::mutex> g(globalMu_);
    if (--instanceCount_ == 0) {
        WSACleanup();
    }
}

bool TcpClient::Connect(const std::string& host, int port, std::string* errorOut) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        if (errorOut) *errorOut = wsaErr("getaddrinfo", rc);
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        if (errorOut) *errorOut = wsaErr("connect", WSAGetLastError());
        return false;
    }

    // TCP_NODELAY: video frames are bursty and small control frames must not
    // sit in Nagle's buffer waiting for company.
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    // SO_SNDBUF small (~128KB). Windows defaults the send buffer to ~2MB+
    // (auto-tuned), which means WriteAll returns immediately even when the
    // far side is several seconds behind reading — frames silently pile up
    // in kernel memory. For a low-latency screen stream we want WriteAll
    // to BLOCK as soon as the consumer falls behind, so the encoder gets
    // back-pressure and we never emit a frame the viewer won't see for ~1s.
    // 128KB ≈ 2 typical encoded frames; one IDR (~500KB) still fits in
    // a single send via TCP segmentation.
    int sndBuf = 128 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));

    sock_ = static_cast<std::uint64_t>(s);
    return true;
}

void TcpClient::Close() {
    if (sock_ != kInvalid) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = kInvalid;
    }
}

bool TcpClient::WriteAll(const std::uint8_t* data, std::size_t size, std::string* errorOut) {
    std::lock_guard<std::mutex> g(writeMu_);
    if (sock_ == kInvalid) {
        if (errorOut) *errorOut = "socket not open";
        return false;
    }
    SOCKET s = static_cast<SOCKET>(sock_);
    std::size_t sent = 0;
    while (sent < size) {
        int n = send(s, reinterpret_cast<const char*>(data + sent),
                     static_cast<int>(size - sent), 0);
        if (n == SOCKET_ERROR) {
            if (errorOut) *errorOut = wsaErr("send", WSAGetLastError());
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpClient::ReadFull(std::uint8_t* buf, std::size_t size, std::string* errorOut) {
    if (sock_ == kInvalid) {
        if (errorOut) *errorOut = "socket not open";
        return false;
    }
    SOCKET s = static_cast<SOCKET>(sock_);
    std::size_t got = 0;
    while (got < size) {
        int n = recv(s, reinterpret_cast<char*>(buf + got),
                     static_cast<int>(size - got), 0);
        if (n == 0) {
            if (errorOut) *errorOut = "connection closed by peer";
            return false;
        }
        if (n == SOCKET_ERROR) {
            if (errorOut) *errorOut = wsaErr("recv", WSAGetLastError());
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}
