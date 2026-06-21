#include "net/frame.h"

#include <cctype>
#include <sstream>

namespace proto {

namespace {
void writeBE32(std::uint8_t* out, std::uint32_t v) {
    out[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(v & 0xFF);
}
std::uint32_t readBE32(const std::uint8_t* in) {
    return (std::uint32_t(in[0]) << 24) | (std::uint32_t(in[1]) << 16) |
           (std::uint32_t(in[2]) << 8) | std::uint32_t(in[3]);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}
} // namespace

bool ReadFrame(TcpClient& c, std::uint8_t* typeOut,
               std::vector<std::uint8_t>* payloadOut, std::string* errOut) {
    std::uint8_t header[kHeaderSize];
    if (!c.ReadFull(header, sizeof(header), errOut)) return false;
    const std::uint32_t length = readBE32(header + 1);
    if (length > kMaxPayloadSize) {
        if (errOut) *errOut = "frame payload exceeds maximum size";
        return false;
    }
    payloadOut->resize(length);
    if (length > 0) {
        if (!c.ReadFull(payloadOut->data(), length, errOut)) return false;
    }
    *typeOut = header[0];
    return true;
}

bool WriteFrame(TcpClient& c, std::uint8_t type,
                const std::uint8_t* payload, std::size_t payloadSize,
                std::string* errOut) {
    if (payloadSize > kMaxPayloadSize) {
        if (errOut) *errOut = "frame payload exceeds maximum size";
        return false;
    }
    std::uint8_t header[kHeaderSize];
    header[0] = type;
    writeBE32(header + 1, static_cast<std::uint32_t>(payloadSize));

    // Glue header + payload into a single send() to avoid an extra TCP segment.
    std::vector<std::uint8_t> buf;
    buf.reserve(kHeaderSize + payloadSize);
    buf.insert(buf.end(), header, header + kHeaderSize);
    if (payloadSize > 0) {
        buf.insert(buf.end(), payload, payload + payloadSize);
    }
    return c.WriteAll(buf.data(), buf.size(), errOut);
}

std::string EncodeRegisterPayload(const std::string& deviceId,
                                  const std::string& deviceToken) {
    std::ostringstream oss;
    oss << "{\"device_id\":\"" << jsonEscape(deviceId)
        << "\",\"device_token\":\"" << jsonEscape(deviceToken) << "\"}";
    return oss.str();
}

std::string EncodeAuthPayload(const std::string& account,
                              const std::string& sessionToken) {
    std::ostringstream oss;
    oss << "{\"account\":\"" << jsonEscape(account)
        << "\",\"session_token\":\"" << jsonEscape(sessionToken) << "\"}";
    return oss.str();
}

std::string EncodeConnectPayload(const std::string& deviceId) {
    std::ostringstream oss;
    oss << "{\"device_id\":\"" << jsonEscape(deviceId) << "\"}";
    return oss.str();
}

bool ParseResponseJSON(const std::uint8_t* data, std::size_t size,
                       bool* ok, std::string* message) {
    if (ok) *ok = false;
    if (message) message->clear();

    const std::string s(reinterpret_cast<const char*>(data), size);

    auto findKey = [&](const std::string& key) -> std::size_t {
        const std::string needle = "\"" + key + "\"";
        std::size_t p = s.find(needle);
        if (p == std::string::npos) return std::string::npos;
        p = s.find(':', p + needle.size());
        if (p == std::string::npos) return std::string::npos;
        return p + 1;
    };

    std::size_t okPos = findKey("ok");
    if (okPos == std::string::npos) return false;
    while (okPos < s.size() && std::isspace(static_cast<unsigned char>(s[okPos]))) ++okPos;
    if (s.compare(okPos, 4, "true") == 0) {
        if (ok) *ok = true;
    } else if (s.compare(okPos, 5, "false") == 0) {
        if (ok) *ok = false;
    } else {
        return false;
    }

    std::size_t msgPos = findKey("message");
    if (msgPos != std::string::npos && message) {
        while (msgPos < s.size() && std::isspace(static_cast<unsigned char>(s[msgPos]))) ++msgPos;
        if (msgPos < s.size() && s[msgPos] == '"') {
            ++msgPos;
            const std::size_t end = s.find('"', msgPos);
            if (end != std::string::npos) {
                *message = s.substr(msgPos, end - msgPos);
            }
        }
    }
    return true;
}

} // namespace proto
