#pragma once

// Minimal blocking TCP client over Winsock. Write is mutex-protected so
// multiple threads can SendFrame concurrently without interleaving. Read is
// single-threaded — the caller's read loop is the only reader.
//
// Lifted out of agent/viewer net/ to a common place so both sides can link
// the same symbol — and so the future app/ that wraps both can be a single
// exe without duplicate-symbol link errors.

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

    // Writes the whole buffer or fails. Thread-safe.
    bool WriteAll(const std::uint8_t* data, std::size_t size, std::string* errorOut);

    // Reads exactly `size` bytes. NOT thread-safe — designate a single reader.
    bool ReadFull(std::uint8_t* buf, std::size_t size, std::string* errorOut);

private:
    // SOCKET is a typedef for UINT_PTR in winsock; keep it opaque here so we
    // don't have to drag <winsock2.h> into every consumer.
    std::uint64_t sock_;
    std::mutex writeMu_;
    static int instanceCount_;
    static std::mutex globalMu_;
};
