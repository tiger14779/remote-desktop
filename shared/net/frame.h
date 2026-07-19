#pragma once

// Wire protocol shared by agent, viewer, and the unified app. Frame layout:
//
//   [1 byte type] [4 byte big-endian length] [payload bytes]
//
// All MessageType constants and the byte-level Read/Write/Parse helpers live
// here so both connection sides (Agent → REGISTER, Viewer → AUTH + CONNECT)
// can be linked into a single binary without duplicate-symbol issues.

#include "net/tcp_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace proto {

enum MessageType : std::uint8_t {
    MsgRegister      = 0x01,
    MsgAuth          = 0x02,
    MsgConnect       = 0x03,
    MsgSessionStart  = 0x04,
    MsgSessionEnd    = 0x05,

    MsgVideoFrame    = 0x10,
    MsgMouseMove     = 0x20,
    MsgMouseButton   = 0x21,
    MsgKeyEvent      = 0x22,
    // Vertical scroll wheel. Payload: [i16 BE delta] in WHEEL_DELTA units
    // (one physical notch = 120, positive = scroll away from user / up).
    MsgMouseWheel    = 0x23,
    MsgHeartbeat     = 0x30,
    MsgClipboardText = 0x40,  // payload: UTF-8 bytes, no terminator
    MsgCursorShape   = 0x50,  // payload: width(2BE)+height(2BE)+hotX(2BE)+hotY(2BE)+BGRA

    // File transfer over clipboard. One transfer = one Begin, N Chunks
    // (sequential, no fileIdx — receiver tracks by bytes-remaining-per-file),
    // one End. No transferId either, since at most one xfer is in-flight on
    // a connection (the relay's TCP stream is ordered).
    MsgFileXferBegin = 0x41,  // payload: JSON {"files":[{"path":"...","size":N,"isDir":bool},...]}
    MsgFileXferChunk = 0x42,  // payload: raw bytes; appended to current file
    MsgFileXferEnd   = 0x43,  // payload: JSON {"ok":bool} (empty {} for success)

    // UDP data plane handshake: relay -> peer right after session start.
    // Payload: JSON {"session_id":uint32, "host":string, "port":int}. Empty
    // host means "reuse the TCP relay address".
    MsgUdpInfo       = 0x60,
    // P2P NAT-T candidate exchange. Two flavours, same payload shape:
    //   MsgUdpCandidates   (0x61) — relay -> peer, "here's the OTHER peer's
    //                                reflex endpoint (as I see it)".
    //   MsgUdpMyCandidates (0x62) — peer -> peer via relay forward, "here
    //                                are MY local LAN endpoints, try them
    //                                too". Relay is transparent for this
    //                                type; it uses the same forwarding path
    //                                as clipboard / file-xfer.
    // Payload for both: JSON
    //   {"candidates": [{"host":"1.2.3.4","port":51849}, ...]}
    MsgUdpCandidates   = 0x61,
    MsgUdpMyCandidates = 0x62,
    // Video plane control, viewer -> agent (relay blind-forwards viewer
    // frames, so no relay change is needed). Both carry an empty payload.
    //   MsgRequestIdr  — viewer lost UDP frame(s); agent should force an IDR
    //                    on its next encode so the picture resyncs.
    //   MsgUseTcpVideo — viewer's UDP watchdog saw no video for several
    //                    seconds; agent should drop the UDP xport and send
    //                    video over the TCP relay path for the rest of the
    //                    session (low-bandwidth encoder profile).
    MsgRequestIdr      = 0x63,
    MsgUseTcpVideo     = 0x64,

    MsgRegisterResp  = 0x81,
    MsgAuthResp      = 0x82,
    MsgConnectResp   = 0x83,
    MsgError         = 0xFE,
};

inline const char* MessageTypeName(std::uint8_t t) {
    switch (t) {
        case MsgRegister:     return "REGISTER";
        case MsgAuth:         return "AUTH";
        case MsgConnect:      return "CONNECT";
        case MsgSessionStart: return "SESSION_START";
        case MsgSessionEnd:   return "SESSION_END";
        case MsgVideoFrame:   return "VIDEO_FRAME";
        case MsgMouseMove:    return "MOUSE_MOVE";
        case MsgMouseButton:  return "MOUSE_BUTTON";
        case MsgKeyEvent:     return "KEY_EVENT";
        case MsgMouseWheel:   return "MOUSE_WHEEL";
        case MsgHeartbeat:    return "HEARTBEAT";
        case MsgClipboardText: return "CLIPBOARD_TEXT";
        case MsgCursorShape:  return "CURSOR_SHAPE";
        case MsgFileXferBegin: return "FILE_XFER_BEGIN";
        case MsgFileXferChunk: return "FILE_XFER_CHUNK";
        case MsgFileXferEnd:   return "FILE_XFER_END";
        case MsgUdpInfo:       return "UDP_INFO";
        case MsgUdpCandidates:   return "UDP_CANDIDATES";
        case MsgUdpMyCandidates: return "UDP_MY_CANDIDATES";
        case MsgRequestIdr:      return "REQUEST_IDR";
        case MsgUseTcpVideo:     return "USE_TCP_VIDEO";
        case MsgRegisterResp: return "REGISTER_RESP";
        case MsgAuthResp:     return "AUTH_RESP";
        case MsgConnectResp:  return "CONNECT_RESP";
        case MsgError:        return "ERROR";
        default:              return "UNKNOWN";
    }
}

constexpr int kHeaderSize = 5;
constexpr std::size_t kMaxPayloadSize = 16ull * 1024 * 1024;

bool ReadFrame(TcpClient& c, std::uint8_t* typeOut,
               std::vector<std::uint8_t>* payloadOut, std::string* errOut);

bool WriteFrame(TcpClient& c, std::uint8_t type,
                const std::uint8_t* payload, std::size_t payloadSize,
                std::string* errOut);

// JSON encoders — both sides live here so a single exe can link both
// connection paths without symbol collisions on a "proto" namespace.
std::string EncodeRegisterPayload(const std::string& deviceId,
                                  const std::string& deviceToken);

std::string EncodeAuthPayload(const std::string& account,
                              const std::string& sessionToken);

std::string EncodeConnectPayload(const std::string& deviceId);

// Parse a generic Response JSON ({"ok":bool,"message":string}).
bool ParseResponseJSON(const std::uint8_t* data, std::size_t size,
                       bool* ok, std::string* message);

} // namespace proto
