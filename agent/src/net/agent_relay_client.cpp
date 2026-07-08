#include "net/agent_relay_client.h"

#include "net/frame.h"

#include <cstring>
#include <iostream>
#include <vector>

bool RelayClient::ConnectAndRegister(const Config& cfg, std::string* errorOut) {
    if (!sock_.Connect(cfg.host, cfg.port, errorOut)) return false;

    const std::string payload = proto::EncodeRegisterPayload(cfg.deviceId, cfg.deviceToken);
    if (!proto::WriteFrame(sock_, proto::MsgRegister,
                           reinterpret_cast<const std::uint8_t*>(payload.data()),
                           payload.size(), errorOut)) {
        return false;
    }

    std::uint8_t type = 0;
    std::vector<std::uint8_t> body;
    if (!proto::ReadFrame(sock_, &type, &body, errorOut)) return false;
    if (type != proto::MsgRegisterResp) {
        if (errorOut) *errorOut = std::string("expected REGISTER_RESP, got ") +
                                  proto::MessageTypeName(type);
        return false;
    }
    bool ok = false;
    std::string msg;
    if (!proto::ParseResponseJSON(body.data(), body.size(), &ok, &msg)) {
        if (errorOut) *errorOut = "could not parse REGISTER_RESP";
        return false;
    }
    if (!ok) {
        if (errorOut) *errorOut = "relay refused REGISTER: " + msg;
        return false;
    }
    return true;
}

void RelayClient::Run(const Callbacks& cb) {
    while (true) {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> body;
        std::string err;
        if (!proto::ReadFrame(sock_, &type, &body, &err)) {
            std::cerr << "relay read: " << err << "\n";
            return;
        }
        switch (type) {
            case proto::MsgSessionStart:
                if (cb.onSessionStart) cb.onSessionStart();
                break;
            case proto::MsgSessionEnd:
                if (cb.onSessionEnd) cb.onSessionEnd();
                break;
            case proto::MsgMouseMove:
            case proto::MsgMouseButton:
            case proto::MsgKeyEvent:
            case proto::MsgMouseWheel:
                if (cb.onInput) cb.onInput(type, body.data(), body.size());
                break;
            case proto::MsgClipboardText:
                if (cb.onClipboard) cb.onClipboard(body.data(), body.size());
                break;
            case proto::MsgFileXferBegin:
            case proto::MsgFileXferChunk:
            case proto::MsgFileXferEnd:
                if (cb.onFileXfer) cb.onFileXfer(type, body.data(), body.size());
                break;
            case proto::MsgUdpInfo:
                if (cb.onUdpInfo) cb.onUdpInfo(body.data(), body.size());
                break;
            case proto::MsgUdpCandidates:
                if (cb.onUdpCandidates) cb.onUdpCandidates(body.data(), body.size());
                break;
            case proto::MsgUdpMyCandidates:
                if (cb.onUdpPeerLan) cb.onUdpPeerLan(body.data(), body.size());
                break;
            case proto::MsgHeartbeat:
                // Relay reflects ours; nothing to do.
                break;
            default:
                std::cerr << "relay: unexpected frame "
                          << proto::MessageTypeName(type) << "\n";
                break;
        }
    }
}

bool RelayClient::SendVideoFrame(const std::uint8_t* data, std::size_t size,
                                 std::string* errorOut) {
    return proto::WriteFrame(sock_, proto::MsgVideoFrame, data, size, errorOut);
}

bool RelayClient::SendClipboard(const std::uint8_t* data, std::size_t size,
                                std::string* errorOut) {
    return proto::WriteFrame(sock_, proto::MsgClipboardText, data, size, errorOut);
}

bool RelayClient::SendCursorShape(int width, int height, int hotX, int hotY,
                                   const std::uint8_t* bgra, std::size_t bgraSize,
                                   std::string* errorOut) {
    std::vector<std::uint8_t> payload(8 + bgraSize);
    auto u = [](int v) { return static_cast<std::uint16_t>(v < 0 ? 0 : v); };
    auto w = u(width), h = u(height), hx = u(hotX), hy = u(hotY);
    payload[0] = (w >> 8) & 0xFF; payload[1] = w & 0xFF;
    payload[2] = (h >> 8) & 0xFF; payload[3] = h & 0xFF;
    payload[4] = (hx >> 8) & 0xFF; payload[5] = hx & 0xFF;
    payload[6] = (hy >> 8) & 0xFF; payload[7] = hy & 0xFF;
    if (bgraSize > 0) std::memcpy(payload.data() + 8, bgra, bgraSize);
    return proto::WriteFrame(sock_, proto::MsgCursorShape,
                              payload.data(), payload.size(), errorOut);
}

bool RelayClient::SendFrameRaw(std::uint8_t type,
                                const std::uint8_t* data, std::size_t size,
                                std::string* errorOut) {
    return proto::WriteFrame(sock_, type, data, size, errorOut);
}

bool RelayClient::SendHeartbeat(std::string* errorOut) {
    return proto::WriteFrame(sock_, proto::MsgHeartbeat, nullptr, 0, errorOut);
}

void RelayClient::Close() { sock_.Close(); }
