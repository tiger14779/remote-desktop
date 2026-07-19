#include "udp/video_xport.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

// Wire constants — must match relay/internal/udprelay/forward.go and the C++
// peer.
constexpr std::uint8_t kPktVideoFrag = 0x01;
constexpr std::uint8_t kPktRegister  = 0x02;
constexpr std::uint8_t kPktProbe     = 0x04;  // P2P NAT-T probe (client -> peer)
constexpr std::uint8_t kPktProbeAck  = 0x05;  // P2P NAT-T probe ack (peer -> client)

// 4-byte session_id + 1-byte type + 4-byte frame_id + 2-byte frag_idx
// + 2-byte frag_count = 13 bytes total wire header per video fragment.
constexpr std::size_t kVideoHeaderSize = 4 + 1 + 4 + 2 + 2;
constexpr std::size_t kMaxPacketSize   = 1200;  // safe MTU minus headers
constexpr std::size_t kMaxFragData     = kMaxPacketSize - kVideoHeaderSize;

// Discard a half-received frame this long after the first fragment landed.
// 500ms is generous (typical drain is < 1 RTT) and bounds memory growth
// under persistent loss to one frame's worth per ~half second.
constexpr auto kFrameAssemblyTimeout = std::chrono::milliseconds(500);

// Frames older than this many ids behind the highest-delivered get GC'd
// even if some chunks are still pending — they're stale and we'd just be
// keeping the decoder waiting.
constexpr std::uint32_t kFrameIdGcWindow = 30;

inline void writeBE32(std::uint8_t* p, std::uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF; p[3] = (v      ) & 0xFF;
}
inline void writeBE16(std::uint8_t* p, std::uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}
inline std::uint32_t readBE32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
}
inline std::uint16_t readBE16(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

} // namespace

VideoUdpXport::VideoUdpXport() = default;

VideoUdpXport::~VideoUdpXport() { Stop(); }

bool VideoUdpXport::Init(const std::string& relayHost, int relayPort,
                          std::uint32_t sessionId, Role role,
                          std::string* errorOut) {
    relay_.host = relayHost;
    relay_.port = relayPort;
    dst_ = relay_;
    sessionId_ = sessionId;
    role_ = role;
    if (!sock_.Bind("0.0.0.0", 0, errorOut)) return false;
    return true;
}

int VideoUdpXport::LocalPort() const { return sock_.LocalPort(); }

void VideoUdpXport::SetRelay(const std::string& host, int port) {
    std::lock_guard<std::mutex> g(dstMu_);
    relay_.host = host;
    relay_.port = port;
    if (!directUp_.load()) {
        // Not yet direct — outbound still targets the relay, so reroute.
        dst_ = relay_;
    }
}

void VideoUdpXport::AddDirectCandidate(const std::string& peerHost, int peerPort) {
    if (directUp_.load()) return;  // already picked a winner
    UdpSocket::Endpoint ep;
    ep.host = peerHost;
    ep.port = peerPort;
    {
        std::lock_guard<std::mutex> g(dstMu_);
        // De-dup: a peer that reports the same LAN IP twice (rare, but
        // possible when both come from different messages) shouldn't cause
        // us to double-probe.
        for (const auto& e : peerCandidates_) {
            if (e.host == ep.host && e.port == ep.port) return;
        }
        peerCandidates_.push_back(ep);
    }
    // 5-second window is our punching budget from THIS add — a late arrival
    // (e.g., peer's LAN candidate message shows up 100ms after the relay
    // reflex one) refreshes the deadline so it gets a fair chance.
    probeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

void VideoUdpXport::StartReceiver(FrameReadyFn cb) {
    frameCb_ = std::move(cb);
    shouldExit_ = false;
    recvThread_ = std::thread([this]() { recvLoop(); });
}

void VideoUdpXport::Stop() {
    shouldExit_ = true;
    sock_.Close();
    if (recvThread_.joinable()) recvThread_.join();
}

bool VideoUdpXport::SendFrame(const std::uint8_t* data, std::size_t size,
                               std::string* errorOut) {
    if (size == 0) return true;
    // Snapshot the destination — could be relay (initial) or peer (after
    // NAT-T success). Send while holding a copy so a concurrent SetRelay
    // or direct-swap can't race the sendto.
    UdpSocket::Endpoint dst;
    {
        std::lock_guard<std::mutex> g(dstMu_);
        dst = dst_;
    }
    const std::uint32_t frameId = nextFrameId_++;
    const std::size_t fragCount = (size + kMaxFragData - 1) / kMaxFragData;
    if (fragCount > 0xFFFF) {
        if (errorOut) *errorOut = "frame too large to fragment in uint16 index";
        return false;
    }

    std::uint8_t pkt[kMaxPacketSize];
    for (std::size_t i = 0; i < fragCount; ++i) {
        const std::size_t offset = i * kMaxFragData;
        const std::size_t chunk  = std::min(kMaxFragData, size - offset);

        writeBE32(&pkt[0], sessionId_);
        pkt[4] = kPktVideoFrag;
        writeBE32(&pkt[5], frameId);
        writeBE16(&pkt[9], static_cast<std::uint16_t>(i));
        writeBE16(&pkt[11], static_cast<std::uint16_t>(fragCount));
        std::memcpy(&pkt[13], data + offset, chunk);

        std::string e;
        if (!sock_.SendTo(dst, pkt, kVideoHeaderSize + chunk, &e)) {
            // Don't fail the whole frame on a transient send error — the
            // viewer will just drop this frame. But surface it once for
            // diagnostics.
            if (errorOut) *errorOut = e;
            return false;
        }
    }
    return true;
}

bool VideoUdpXport::SendRegister(std::string* errorOut) {
    // REGISTER always goes to the relay (not the peer) — the relay is the
    // only party that cares about the endpoint mapping. Payload layout:
    //   [4B session_id][1B type=REGISTER][1B role: 0=agent, 1=viewer]
    std::uint8_t pkt[6];
    writeBE32(&pkt[0], sessionId_);
    pkt[4] = kPktRegister;
    pkt[5] = static_cast<std::uint8_t>(role_);
    return sock_.SendTo(relay_, pkt, sizeof(pkt), errorOut);
}

void VideoUdpXport::recvLoop() {
    std::vector<std::uint8_t> buf(64 * 1024);
    UdpSocket::Endpoint src;
    // First REGISTER goes out immediately so the relay learns our endpoint
    // before any data can arrive. Subsequent ones every 3s keep the NAT
    // mapping along the path warm — most home routers drop UDP bindings
    // after 30-60 seconds of silence.
    constexpr auto kRegisterInterval = std::chrono::seconds(3);
    // Probe cadence during the NAT-T punching window: several sends per
    // second so the "simultaneous open" window a router NAT gives us has
    // room to succeed. Cheap — a probe is 5 bytes.
    constexpr auto kProbeInterval    = std::chrono::milliseconds(150);
    std::string regErr;
    SendRegister(&regErr);
    auto lastRegister = std::chrono::steady_clock::now();
    auto lastProbe    = std::chrono::steady_clock::time_point::min();

    // Frame-loss reporting. All state lives on this thread. Rate-limited so
    // sustained loss produces a steady trickle of callbacks (the viewer turns
    // each into a REQUEST_IDR), not a storm.
    constexpr auto kLossCbMinInterval = std::chrono::milliseconds(250);
    auto lastLossCb = std::chrono::steady_clock::time_point{};
    auto reportLoss = [&](std::chrono::steady_clock::time_point now) {
        if (!lossCb_) return;
        if (now - lastLossCb < kLossCbMinInterval) return;
        lastLossCb = now;
        lossCb_();
    };
    // Evict stale half-assembled frames on a steady cadence. The old code
    // only evicted inside the recv-timeout branch, which under continuous
    // (lossy) traffic never fires — incomplete frames would linger until a
    // quiet moment.
    auto lastEvict = std::chrono::steady_clock::now();

    while (!shouldExit_.load()) {
        std::string err;
        const int got = sock_.RecvFrom(buf.data(), buf.size(),
                                        &src, /*timeoutMs=*/200, &err);

        const auto now = std::chrono::steady_clock::now();

        // Re-register if it's time, regardless of recv result.
        if (now - lastRegister >= kRegisterInterval) {
            std::string e;
            SendRegister(&e);
            lastRegister = now;
        }

        // Send periodic NAT-T probes to every candidate while the window is
        // open and we don't yet have a confirmed direct path. LAN
        // candidates are near-free — they'll either punch through instantly
        // (same-subnet case) or vanish (routing sends them nowhere).
        if (!directUp_.load() && now - lastProbe >= kProbeInterval) {
            std::vector<UdpSocket::Endpoint> cands;
            {
                std::lock_guard<std::mutex> g(dstMu_);
                if (now < probeUntil_) cands = peerCandidates_;
            }
            if (!cands.empty()) {
                std::uint8_t probe[5];
                writeBE32(&probe[0], sessionId_);
                probe[4] = kPktProbe;
                for (const auto& c : cands) {
                    std::string e;
                    sock_.SendTo(c, probe, sizeof(probe), &e);
                }
                lastProbe = now;
            }
        }

        // Periodic stale-frame eviction (runs whether or not a datagram
        // arrived this iteration). An evicted incomplete frame is definite
        // loss — some of its fragments never made it.
        if (now - lastEvict >= std::chrono::milliseconds(100)) {
            lastEvict = now;
            bool evicted = false;
            {
                std::lock_guard<std::mutex> g(pendingMu_);
                for (auto it = pending_.begin(); it != pending_.end(); ) {
                    if (now - it->second.firstSeen > kFrameAssemblyTimeout) {
                        it = pending_.erase(it);
                        evicted = true;
                    } else {
                        ++it;
                    }
                }
            }
            if (evicted) reportLoss(now);
        }

        if (got < 0) {
            if (shouldExit_.load()) return;
            // Hard error — log once a second of throttled noise would mean.
            // Close + reopen logic could go here later. For now just bail.
            std::cerr << "[udp] recvLoop: " << err << "\n";
            return;
        }
        if (got == 0) continue;  // recv timeout — nothing else to do
        if (got < 5) continue;

        const std::uint32_t sid = readBE32(&buf[0]);
        const std::uint8_t  typ = buf[4];
        if (sid != sessionId_) continue;

        // NAT-T probe from the peer. Send an ack back to whatever source
        // address it came from — that's the working return path through
        // the peer's NAT. The ack is 5 bytes; we deliberately don't
        // rate-limit these so a probe burst gets a matching ack burst.
        if (typ == kPktProbe) {
            std::uint8_t ack[5];
            writeBE32(&ack[0], sessionId_);
            ack[4] = kPktProbeAck;
            std::string e;
            sock_.SendTo(src, ack, sizeof(ack), &e);
            continue;
        }

        // The peer's ack for one of our probes. First one wins: mark direct
        // up, swap the send destination to the peer, fire the callback so
        // the GUI can log it. Subsequent acks are ignored.
        if (typ == kPktProbeAck) {
            bool alreadyUp = directUp_.exchange(true);
            if (!alreadyUp) {
                {
                    std::lock_guard<std::mutex> g(dstMu_);
                    dst_ = src;  // switch outbound to direct peer
                }
                if (directCb_) directCb_(src.host, src.port);
            }
            continue;
        }

        if (typ != kPktVideoFrag) continue;
        if (got < static_cast<int>(kVideoHeaderSize)) continue;

        const std::uint32_t frameId  = readBE32(&buf[5]);
        const std::uint16_t fragIdx  = readBE16(&buf[9]);
        const std::uint16_t fragCnt  = readBE16(&buf[11]);
        if (fragCnt == 0 || fragIdx >= fragCnt) continue;
        // Anything older than the GC window is stale; drop instead of
        // queueing chunks that the decoder will never get to see.
        if (highestDelivered_ > frameId &&
            highestDelivered_ - frameId > kFrameIdGcWindow) continue;

        const std::size_t dataLen = static_cast<std::size_t>(got) - kVideoHeaderSize;
        const std::uint8_t* dataPtr = &buf[kVideoHeaderSize];

        std::vector<std::uint8_t> assembled;
        bool deliver = false;
        bool gap = false;
        {
            std::lock_guard<std::mutex> g(pendingMu_);
            auto& pf = pending_[frameId];
            if (pf.expected == 0) {
                pf.expected  = fragCnt;
                pf.chunks.resize(fragCnt);
                pf.firstSeen = std::chrono::steady_clock::now();
            }
            // Duplicate fragment — ignore the second copy. UDP can dup.
            if (!pf.chunks[fragIdx].empty()) continue;
            pf.chunks[fragIdx].assign(dataPtr, dataPtr + dataLen);
            pf.received++;
            if (pf.received == pf.expected) {
                std::size_t total = 0;
                for (auto& c : pf.chunks) total += c.size();
                assembled.reserve(total);
                for (auto& c : pf.chunks) {
                    assembled.insert(assembled.end(), c.begin(), c.end());
                }
                pending_.erase(frameId);
                if (frameId > highestDelivered_) {
                    // Skipped ids = frames whose fragments never completed.
                    // (Out-of-order completion can false-positive here; the
                    // cost is one rate-limited extra IDR, acceptable.)
                    gap = highestDelivered_ != 0 &&
                          frameId > highestDelivered_ + 1;
                    highestDelivered_ = frameId;
                }
                deliver = true;
            }
        }
        if (gap) reportLoss(now);
        if (deliver && frameCb_) {
            frameCb_(assembled.data(), assembled.size());
        }
    }
}
