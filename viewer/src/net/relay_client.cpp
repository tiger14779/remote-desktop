#include "net/relay_client.h"

#include "net/frame.h"

#include <iostream>

namespace {
bool exchange(TcpClient& sock,
              std::uint8_t outType, const std::string& outPayload,
              std::uint8_t wantInType, std::string* err) {
    if (!proto::WriteFrame(sock, outType,
                           reinterpret_cast<const std::uint8_t*>(outPayload.data()),
                           outPayload.size(), err)) {
        return false;
    }
    std::uint8_t inType = 0;
    std::vector<std::uint8_t> inBody;
    if (!proto::ReadFrame(sock, &inType, &inBody, err)) return false;
    if (inType != wantInType) {
        if (err) *err = std::string("expected ") + proto::MessageTypeName(wantInType) +
                        ", got " + proto::MessageTypeName(inType);
        return false;
    }
    bool ok = false;
    std::string msg;
    if (!proto::ParseResponseJSON(inBody.data(), inBody.size(), &ok, &msg)) {
        if (err) *err = "could not parse response JSON";
        return false;
    }
    if (!ok) {
        if (err) *err = "relay rejected: " + msg;
        return false;
    }
    return true;
}
} // namespace

bool ViewerRelayClient::ConnectAuthorize(const Config& cfg, std::string* errorOut) {
    if (!sock_.Connect(cfg.host, cfg.port, errorOut)) return false;
    if (!exchange(sock_, proto::MsgAuth,
                  proto::EncodeAuthPayload(cfg.account, cfg.sessionToken),
                  proto::MsgAuthResp, errorOut)) {
        return false;
    }
    if (!exchange(sock_, proto::MsgConnect,
                  proto::EncodeConnectPayload(cfg.deviceId),
                  proto::MsgConnectResp, errorOut)) {
        return false;
    }
    return true;
}

void ViewerRelayClient::Run(const Callbacks& cb) {
    while (true) {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> body;
        std::string err;
        if (!proto::ReadFrame(sock_, &type, &body, &err)) {
            std::cerr << "relay read: " << err << "\n";
            if (cb.onDisconnect) cb.onDisconnect();
            return;
        }
        switch (type) {
            case proto::MsgVideoFrame:
                if (cb.onVideoFrame) cb.onVideoFrame(body.data(), body.size());
                break;
            case proto::MsgHeartbeat:
                break;
            default:
                std::cerr << "relay: unexpected frame "
                          << proto::MessageTypeName(type) << "\n";
                break;
        }
    }
}

bool ViewerRelayClient::SendInput(std::uint8_t type, const std::uint8_t* data,
                                  std::size_t size, std::string* errorOut) {
    return proto::WriteFrame(sock_, type, data, size, errorOut);
}

void ViewerRelayClient::Close() { sock_.Close(); }
