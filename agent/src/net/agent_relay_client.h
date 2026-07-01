#pragma once

// Agent-side relay client: handles REGISTER, then runs a read loop that
// dispatches SESSION_START/SESSION_END and input frames via callbacks. Video
// frames are pushed outbound through SendVideoFrame and may be called from a
// different thread than Run().

#include "net/tcp_client.h"

#include <cstdint>
#include <functional>
#include <string>

class RelayClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 7100;
        std::string deviceId;
        std::string deviceToken;
    };

    struct Callbacks {
        std::function<void()> onSessionStart;
        std::function<void()> onSessionEnd;
        // type is the protocol MessageType for the incoming input frame.
        std::function<void(std::uint8_t type,
                           const std::uint8_t* data,
                           std::size_t size)> onInput;
        // Inbound CLIPBOARD_TEXT from viewer. Payload is UTF-8.
        std::function<void(const std::uint8_t* data, std::size_t size)> onClipboard;
        // Inbound FILE_XFER_BEGIN/CHUNK/END from viewer. Type discriminates.
        std::function<void(std::uint8_t type,
                           const std::uint8_t* data,
                           std::size_t size)> onFileXfer;
        // Inbound UDP_INFO from relay: carries session_id + relay UDP port
        // the agent should target for its video data plane. Payload is the
        // JSON body, caller parses.
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpInfo;
        // UDP_CANDIDATES: relay tells us the peer's public UDP endpoint.
        // Agent side kicks off NAT-T probing to that endpoint.
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpCandidates;
        // UDP_MY_CANDIDATES: peer told us (via relay forward) about its own
        // local candidates (LAN IPs) we should also try probing.
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpPeerLan;
    };

    bool ConnectAndRegister(const Config& cfg, std::string* errorOut);

    // Blocks reading frames until the connection ends. Returns when the peer
    // closes or a fatal protocol error occurs.
    void Run(const Callbacks& cb);

    // Thread-safe. Sends a VIDEO_FRAME with the given Annex-B payload.
    bool SendVideoFrame(const std::uint8_t* data, std::size_t size, std::string* errorOut);

    // Thread-safe. Sends CLIPBOARD_TEXT with the given UTF-8 bytes.
    bool SendClipboard(const std::uint8_t* data, std::size_t size, std::string* errorOut);

    // Thread-safe. Sends CURSOR_SHAPE: 8-byte header (width, height, hotX,
    // hotY — all 2-byte big-endian) followed by width*height*4 BGRA bytes.
    bool SendCursorShape(int width, int height, int hotX, int hotY,
                         const std::uint8_t* bgra, std::size_t bgraSize,
                         std::string* errorOut);

    // Thread-safe. Generic frame writer used for FILE_XFER_BEGIN/CHUNK/END
    // (and anything else we don't want a dedicated wrapper for). Caller is
    // responsible for picking a valid MessageType.
    bool SendFrameRaw(std::uint8_t type,
                      const std::uint8_t* data, std::size_t size,
                      std::string* errorOut);

    // Thread-safe. Sends an empty HEARTBEAT frame; the relay reflects it
    // back so we can also detect a dead link via the read loop's timeout.
    bool SendHeartbeat(std::string* errorOut);

    void Close();

private:
    TcpClient sock_;
};
