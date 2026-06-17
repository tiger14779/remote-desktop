#pragma once

// Mirrors agent/src/net/tcp_client.h verbatim — viewer and agent share the
// same wire protocol so the socket wrapper is identical. Kept duplicated for
// now; lift into a shared library once a third consumer shows up.

#include <cstdint>
#include <mutex>
#include <string>

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool Connect(const std::string& host, int port, std::string* errorOut);
    void Close();

    bool WriteAll(const std::uint8_t* data, std::size_t size, std::string* errorOut);
    bool ReadFull(std::uint8_t* buf, std::size_t size, std::string* errorOut);

private:
    std::uint64_t sock_;
    std::mutex writeMu_;
    static int instanceCount_;
    static std::mutex globalMu_;
};
