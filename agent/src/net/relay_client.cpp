#include "net/relay_client.h"

#include "net/frame.h"

#include <iostream>

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
                if (cb.onInput) cb.onInput(type, body.data(), body.size());
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

void RelayClient::Close() { sock_.Close(); }
