#pragma once

// Cross-platform UDP socket wrapper. Single-instance Winsock init shared with
// TcpClient (both pull from the same per-process WSAStartup refcount).
//
// Send/Recv are direct syscalls — no internal buffering, no thread sync. The
// caller's transport layer (reliable_udp.h) does sequencing, retransmit,
// in-order delivery on top.

#include <cstdint>
#include <string>

class UdpSocket {
public:
    struct Endpoint {
        // IPv4 only for the MVP. RustDesk-style dual-stack can come later
        // once we know whether any agent machines actually have public IPv6.
        std::string host;  // "1.2.3.4" — never a hostname; resolve before binding.
        int         port = 0;
    };

    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bind to a local endpoint. Use "0.0.0.0" + port 0 for "any interface,
    // OS-picked ephemeral port" — the usual client side. Pass an explicit
    // host:port for a server-side listener.
    bool Bind(const std::string& localHost, int localPort, std::string* errorOut);

    // Query the locally bound port (useful when we asked for port 0 and
    // need to tell the relay which port we ended up on).
    int LocalPort() const { return localPort_; }

    // Blocking send to a specific peer. Datagram-oriented: one Send() == one
    // datagram on the wire. Returns false on hard error; partial sends are
    // not possible in UDP.
    bool SendTo(const Endpoint& dst,
                const std::uint8_t* data, std::size_t size,
                std::string* errorOut);

    // Blocking receive. Fills *src with the sender's address. timeoutMs <= 0
    // means "block forever"; > 0 sets SO_RCVTIMEO for the call.
    // Returns:
    //   * size > 0 on a packet
    //   * size == 0 on timeout (errorOut empty)
    //   * size == -1 on hard error (errorOut populated)
    int RecvFrom(std::uint8_t* buf, std::size_t bufSize,
                 Endpoint* src,
                 int timeoutMs,
                 std::string* errorOut);

    // Closes the socket. Safe to call multiple times.
    void Close();

private:
    std::uint64_t sock_;  // SOCKET as opaque uint64 — keeps winsock2.h out of consumers
    int           localPort_ = 0;
};
