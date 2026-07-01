#pragma once

// Viewer-side relay client. Connect → AUTH → CONNECT → frame-receive loop.
// Frame callbacks fire on incoming VIDEO_FRAME; control frames (input frames
// from agent are not expected, but we forward via onInput should they appear).

#include "net/tcp_client.h"

#include <cstdint>
#include <functional>
#include <string>

class ViewerRelayClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 7100;
        std::string account;
        std::string sessionToken;
        std::string deviceId;
    };

    struct Callbacks {
        std::function<void(const std::uint8_t* data, std::size_t size)> onVideoFrame;
        // Inbound CLIPBOARD_TEXT from agent. Payload is UTF-8.
        std::function<void(const std::uint8_t* data, std::size_t size)> onClipboard;
        // Inbound CURSOR_SHAPE: header + BGRA. Caller parses the 8-byte
        // header then takes ownership of the remaining bytes.
        std::function<void(int width, int height, int hotX, int hotY,
                           const std::uint8_t* bgra, std::size_t bgraSize)> onCursorShape;
        // Inbound FILE_XFER_BEGIN/CHUNK/END from agent. Type discriminates.
        std::function<void(std::uint8_t type,
                           const std::uint8_t* data,
                           std::size_t size)> onFileXfer;
        // Inbound UDP_INFO from relay (after CONNECT succeeds): JSON payload
        // carrying session_id + the relay's UDP port for the video data plane.
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpInfo;
        // UDP_CANDIDATES: peer's public UDP endpoint (for NAT-T probing).
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpCandidates;
        // UDP_MY_CANDIDATES: peer's LAN endpoints (forwarded by relay).
        std::function<void(const std::uint8_t* data, std::size_t size)> onUdpPeerLan;
        std::function<void()> onDisconnect;
    };

    // Does AUTH then CONNECT. Errors include rejected auth / device offline /
    // device busy — the response message from the relay is forwarded into err.
    bool ConnectAuthorize(const Config& cfg, std::string* errorOut);

    // Blocks until the connection ends; fires onVideoFrame for each VIDEO_FRAME.
    void Run(const Callbacks& cb);

    // Send an input frame (MOUSE_*/KEY_EVENT). Step 5 will wire viewer input
    // capture into this. Thread-safe.
    bool SendInput(std::uint8_t type, const std::uint8_t* data, std::size_t size,
                   std::string* errorOut);

    // Thread-safe. Sends CLIPBOARD_TEXT with the given UTF-8 bytes.
    bool SendClipboard(const std::uint8_t* data, std::size_t size, std::string* errorOut);

    // Thread-safe. Generic frame writer for FILE_XFER_BEGIN/CHUNK/END etc.
    bool SendFrameRaw(std::uint8_t type,
                      const std::uint8_t* data, std::size_t size,
                      std::string* errorOut);

    // Thread-safe. Empty HEARTBEAT frame; keeps the relay's idle timer fresh.
    bool SendHeartbeat(std::string* errorOut);

    void Close();

private:
    TcpClient sock_;
};
