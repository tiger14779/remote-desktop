#pragma once

// VideoUdpXport: one-channel best-effort video transport over UDP.
//
// Send side:
//   - Caller hands us a complete H.264 frame (Annex-B bytes).
//   - We chunk it into ≤ ~1200 byte fragments, prepend a small header
//     (session_id, frame_id, fragment index, fragment count, type=VIDEO_FRAG)
//     and sendto() each.
//   - No ACK, no retransmit. If a fragment drops, the receiver discards the
//     whole frame; intra-refresh in the encoder repairs the picture within
//     gop_size frames.
//
// Receive side:
//   - A background thread does blocking recvfrom on the bound socket.
//   - Each datagram is filed under its frame_id; once all fragments arrive
//     they're concatenated and pushed via the FrameReadyFn callback.
//   - Frames whose fragments don't all arrive within a brief window
//     (kFrameAssemblyTimeout) get evicted — keeps memory bounded under
//     persistent packet loss.
//
// REGISTER: a separate small datagram the caller periodically sends so the
// relay knows where to forward incoming traffic for this session, and so the
// NAT mapping along the path stays warm. Sent by the client, not by this
// class — callers pick a cadence that fits their session lifetime.

#include "udp/udp_socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VideoUdpXport {
public:
    // Invoked from the receive thread when a complete H.264 frame has been
    // reassembled. The bytes are an Annex-B stream — identical content to
    // the old TCP MsgVideoFrame payload, ready to feed straight into the
    // decoder.
    using FrameReadyFn = std::function<void(const std::uint8_t* data,
                                             std::size_t size)>;

    VideoUdpXport();
    ~VideoUdpXport();

    // Bind a local UDP socket (any port) and store the relay's UDP address
    // we'll talk to. Must be called before SendFrame / StartReceiver.
    bool Init(const std::string& relayHost, int relayPort,
              std::uint32_t sessionId,
              std::string* errorOut);

    // Spawn the recv loop. Callback fires on the recv thread; the caller is
    // responsible for cross-thread handoff (e.g., a QueuedConnection).
    void StartReceiver(FrameReadyFn cb);

    // Send the entire encoded H.264 frame. Internally fragmented. Returns
    // false only on a hard socket error; "send buffer full" is treated as
    // success because UDP doesn't promise delivery anyway.
    bool SendFrame(const std::uint8_t* data, std::size_t size,
                   std::string* errorOut);

    // Idempotent. Tells the relay "I'm endpoint X for session Y, route
    // partner's traffic here." Call ~every 5s to keep NAT bindings warm.
    bool SendRegister(std::string* errorOut);

    void Stop();

private:
    void recvLoop();
    void deliverIfComplete(std::uint32_t frameId);

    struct PendingFrame {
        std::uint16_t expected = 0;
        std::uint16_t received = 0;
        std::vector<std::vector<std::uint8_t>> chunks;  // indexed by frag_idx
        std::chrono::steady_clock::time_point firstSeen;
    };

    UdpSocket sock_;
    UdpSocket::Endpoint relay_;
    std::uint32_t sessionId_ = 0;
    std::uint32_t nextFrameId_ = 1;

    FrameReadyFn frameCb_;
    std::thread  recvThread_;
    std::atomic<bool> shouldExit_{false};

    std::mutex pendingMu_;
    std::map<std::uint32_t, PendingFrame> pending_;
    std::uint32_t highestDelivered_ = 0;
};
