#pragma once

// C++ mirror of relay/internal/protocol — keep the constants in sync with
// that Go source. Frame layout: [1B type][4B BE length][payload].

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
    MsgHeartbeat     = 0x30,

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
        case MsgHeartbeat:    return "HEARTBEAT";
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

// Encode REGISTER request payload as JSON.
std::string EncodeRegisterPayload(const std::string& deviceId,
                                  const std::string& deviceToken);

// Parse a Response JSON ({"ok":bool,"message":string}). Returns false on parse
// failure; otherwise sets *ok and *message.
bool ParseResponseJSON(const std::uint8_t* data, std::size_t size,
                       bool* ok, std::string* message);

} // namespace proto
