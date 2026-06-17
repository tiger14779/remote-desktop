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
    };

    bool ConnectAndRegister(const Config& cfg, std::string* errorOut);

    // Blocks reading frames until the connection ends. Returns when the peer
    // closes or a fatal protocol error occurs.
    void Run(const Callbacks& cb);

    // Thread-safe. Sends a VIDEO_FRAME with the given Annex-B payload.
    bool SendVideoFrame(const std::uint8_t* data, std::size_t size, std::string* errorOut);

    void Close();

private:
    TcpClient sock_;
};
