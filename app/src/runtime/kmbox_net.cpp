#include "runtime/kmbox_net.h"

#include <QRandomGenerator>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
#include <iostream>

// 命令 ID (从 SDK 的 kmboxNet.h 抠出)
namespace {
    constexpr quint32 CMD_CONNECT        = 0xaf3c2828u;
    constexpr quint32 CMD_MOUSE_MOVE     = 0xaede7345u;
    constexpr quint32 CMD_MOUSE_LEFT     = 0x9823AE8Du;
    constexpr quint32 CMD_MOUSE_MIDDLE   = 0x97a3AE8Du;
    constexpr quint32 CMD_MOUSE_RIGHT    = 0x238d8212u;
    constexpr quint32 CMD_MOUSE_WHEEL    = 0xffeead38u;
    constexpr quint32 CMD_MOUSE_AUTOMOVE = 0xaede7346u;
    constexpr quint32 CMD_KEYBOARD_ALL   = 0x123c2c2fu;

    // 载荷大小 —— 与 SDK 的 struct 布局对齐
    constexpr int SIZE_MOUSE    = 4 + 4 + 4 + 4 + 10 * 4;   // 56
    constexpr int SIZE_KEYBOARD = 1 + 1 + 10;                // 12
}

namespace kmbox {

// UUID 字符串 "1F6E5054" 转 uint32 = 0x1F6E5054 (与 SDK StrToHex 语义一致)
static quint32 parseUuidHex(const QString& hex, bool* ok) {
    bool okLocal = false;
    quint32 v = hex.toUInt(&okLocal, 16);
    if (ok) *ok = okLocal && hex.size() == 8;
    return v;
}

KmBoxNet::KmBoxNet() = default;
KmBoxNet::~KmBoxNet() { close(); }

bool KmBoxNet::init(const QString& ip, quint16 port, const QString& uuidHex,
                    QString* err) {
    close();

    if (!wsaStarted_) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            if (err) *err = QStringLiteral("WSAStartup failed");
            return false;
        }
        wsaStarted_ = true;
    }

    bool uuidOk = false;
    uuidMac_ = parseUuidHex(uuidHex, &uuidOk);
    if (!uuidOk) {
        if (err) *err = QStringLiteral("uuid 必须是 8 位十六进制, 如 1F6E5054 (实际: %1)").arg(uuidHex);
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        if (err) *err = QStringLiteral("socket() 失败: WSA=%1").arg(WSAGetLastError());
        return false;
    }
    // 800ms 接收超时。原 SDK 用 3s 是给"从冷启开机的盒子"留缓冲, 但对
    // 我们:
    //   * 每帧鼠键命令: 盒子真机 LAN 内 <5ms 就 ACK, 3s 是 500 倍冗余
    //   * 掉线检测: 盒子被拔线时 recv 会等 3s 才失败 -> 用户在这 3s 内
    //     以为在走硬件, 但实际所有输入都被 recvfrom 阻塞了
    // 800ms 对 LAN 抖动仍有 100 倍余量, 掉线时后台心跳 3s 一次跑一次
    // ping, 用户很快就能看到警告。
    DWORD timeout = 800;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // Silence Windows' UDP-side "port unreachable" propagation. Without
    // this ioctl, when a previous sendto() to (ip,port) triggers an ICMP
    // port-unreachable, the NEXT recvfrom() on this socket fails
    // immediately with WSAECONNRESET (10054) — even if the box then boots
    // up and starts responding legitimately. SIO_UDP_CONNRESET(FALSE)
    // makes Windows swallow the ICMP notification, so recvfrom() will
    // instead time out normally (WSAETIMEDOUT, 10060), which is a much
    // clearer signal for the user ("box didn't answer" vs "connection to
    // what? reset by whom?").
    BOOL disableConnReset = FALSE;
    DWORD unused = 0;
    // SIO_UDP_CONNRESET is IOC_IN | IOC_VENDOR | 12; the constant is not
    // in older MinGW headers — spell it out defensively.
    constexpr DWORD kSioUdpConnReset = 0x9800000C;
    (void)WSAIoctl(s, kSioUdpConnReset, &disableConnReset, sizeof(disableConnReset),
                    nullptr, 0, &unused, nullptr, nullptr);

    sockaddr_in* addr = new sockaddr_in{};
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    inet_pton(AF_INET, ip.toUtf8().constData(), &addr->sin_addr);

    sock_ = static_cast<quintptr>(s);
    serverAddr_ = addr;
    ip_ = ip;
    port_ = port;
    indexpts_ = 0;
    softButton_ = 0;
    std::memset(&softKb_, 0, sizeof(softKb_));

    // 发 CONNECT (空载荷), 用当前 indexpts_=0
    const quint32 rnd = QRandomGenerator::global()->generate();
    QString sendErr;
    if (!sendCmd(CMD_CONNECT, rnd, nullptr, 0, &sendErr)) {
        if (err) *err = QStringLiteral("CONNECT 失败: %1").arg(sendErr);
        close();
        return false;
    }
    connected_ = true;
    std::cerr << "[kmbox] connected " << ip.toStdString() << ":" << port
              << " uuid=" << uuidHex.toStdString() << "\n";
    return true;
}

void KmBoxNet::close() {
    if (sock_) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = 0;
    }
    if (serverAddr_) {
        delete static_cast<sockaddr_in*>(serverAddr_);
        serverAddr_ = nullptr;
    }
    connected_ = false;
}

bool KmBoxNet::sendCmd(quint32 cmd, quint32 randOrMs,
                       const void* payload, int payloadSize,
                       QString* err) {
    QMutexLocker lk(&mtx_);
    if (sock_ == 0) {
        if (err) *err = QStringLiteral("socket not open");
        return false;
    }

    // CONNECT 是首包保持 indexpts=0; 其它包 pre-increment (和 SDK 逻辑一致)
    if (cmd != CMD_CONNECT) ++indexpts_;

    // 组头 16 字节 (小端序)
    QByteArray pkt;
    pkt.reserve(16 + payloadSize);
    auto append32 = [&](quint32 v) {
        pkt.append(char(v & 0xff));
        pkt.append(char((v >> 8) & 0xff));
        pkt.append(char((v >> 16) & 0xff));
        pkt.append(char((v >> 24) & 0xff));
    };
    append32(uuidMac_);
    append32(randOrMs);
    append32(indexpts_);
    append32(cmd);
    if (payload && payloadSize > 0) {
        pkt.append(static_cast<const char*>(payload), payloadSize);
    }

    SOCKET s = static_cast<SOCKET>(sock_);
    auto* sa = static_cast<sockaddr_in*>(serverAddr_);

    int sent = ::sendto(s, pkt.constData(), pkt.size(), 0,
                        reinterpret_cast<sockaddr*>(sa), sizeof(*sa));
    if (sent == SOCKET_ERROR) {
        if (err) *err = QStringLiteral("sendto 失败 WSA=%1").arg(WSAGetLastError());
        return false;
    }

    // 收响应 (1024 缓冲够了)
    char rxbuf[1024];
    sockaddr_in from{};
    int flen = sizeof(from);
    int got = ::recvfrom(s, rxbuf, sizeof(rxbuf), 0,
                         reinterpret_cast<sockaddr*>(&from), &flen);
    if (got == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (err) *err = QStringLiteral("recvfrom 失败 WSA=%1%2")
                            .arg(e).arg(e == WSAETIMEDOUT ? " (超时, 盒子没回应)" : "");
        return false;
    }
    if (got < 16) {
        if (err) *err = QStringLiteral("响应太短: %1B").arg(got);
        return false;
    }
    return true;
}

bool KmBoxNet::sendMouseCmd(quint32 cmd, quint32 randOrMs,
                            int button, int x, int y, int wheel) {
    // soft_mouse_t 布局: button(4) x(4) y(4) wheel(4) point[10](40)
    quint8 payload[SIZE_MOUSE] = {0};
    auto write32 = [](quint8* p, int v) {
        p[0] = quint8(v & 0xff);
        p[1] = quint8((v >> 8) & 0xff);
        p[2] = quint8((v >> 16) & 0xff);
        p[3] = quint8((v >> 24) & 0xff);
    };
    write32(payload + 0,  button);
    write32(payload + 4,  x);
    write32(payload + 8,  y);
    write32(payload + 12, wheel);
    return sendCmd(cmd, randOrMs, payload, SIZE_MOUSE);
}

bool KmBoxNet::sendKeyboardCmd() {
    quint8 payload[SIZE_KEYBOARD] = {0};
    payload[0] = softKb_.ctrl;
    payload[1] = softKb_.reserved;
    std::memcpy(payload + 2, softKb_.keys, 10);
    return sendCmd(CMD_KEYBOARD_ALL, QRandomGenerator::global()->generate(),
                   payload, SIZE_KEYBOARD);
}

bool KmBoxNet::mouseMoveRel(short dx, short dy) {
    return sendMouseCmd(CMD_MOUSE_MOVE, QRandomGenerator::global()->generate(),
                        softButton_, dx, dy, 0);
}

bool KmBoxNet::mouseMoveAuto(int dx, int dy, int ms) {
    if (ms < 5) ms = 5;
    return sendMouseCmd(CMD_MOUSE_AUTOMOVE, static_cast<quint32>(ms),
                        softButton_, dx, dy, 0);
}

bool KmBoxNet::mouseLeft(bool down) {
    softButton_ = down ? (softButton_ | 0x01) : (softButton_ & ~0x01);
    return sendMouseCmd(CMD_MOUSE_LEFT, QRandomGenerator::global()->generate(),
                        softButton_, 0, 0, 0);
}

bool KmBoxNet::mouseRight(bool down) {
    softButton_ = down ? (softButton_ | 0x02) : (softButton_ & ~0x02);
    return sendMouseCmd(CMD_MOUSE_RIGHT, QRandomGenerator::global()->generate(),
                        softButton_, 0, 0, 0);
}

bool KmBoxNet::mouseMiddle(bool down) {
    softButton_ = down ? (softButton_ | 0x04) : (softButton_ & ~0x04);
    return sendMouseCmd(CMD_MOUSE_MIDDLE, QRandomGenerator::global()->generate(),
                        softButton_, 0, 0, 0);
}

bool KmBoxNet::mouseWheel(int wheel) {
    return sendMouseCmd(CMD_MOUSE_WHEEL, QRandomGenerator::global()->generate(),
                        softButton_, 0, 0, wheel);
}

bool KmBoxNet::keyDown(int hidKey) {
    if (hidKey >= hid::KEY_LEFTCONTROL && hidKey <= hid::KEY_RIGHT_GUI) {
        const quint8 bit = 1 << (hidKey - hid::KEY_LEFTCONTROL);
        softKb_.ctrl |= bit;
    } else {
        for (int i = 0; i < 10; ++i) if (softKb_.keys[i] == hidKey) return sendKeyboardCmd();
        for (int i = 0; i < 10; ++i) {
            if (softKb_.keys[i] == 0) {
                softKb_.keys[i] = quint8(hidKey);
                return sendKeyboardCmd();
            }
        }
        // 溢出: 队列左移, 踢掉最早的
        std::memmove(softKb_.keys, softKb_.keys + 1, 9);
        softKb_.keys[9] = quint8(hidKey);
    }
    return sendKeyboardCmd();
}

bool KmBoxNet::ping(QString* err) {
    // Empty CMD_CONNECT re-registration. Box always ACKs it. Doesn't
    // change our indexpts_ (CONNECT is the special case that keeps
    // indexpts at whatever it is), so we don't perturb the ordering of
    // subsequent input commands.
    const quint32 rnd = QRandomGenerator::global()->generate();
    return sendCmd(0xaf3c2828u /*CMD_CONNECT*/, rnd, nullptr, 0, err);
}

bool KmBoxNet::keyUp(int hidKey) {
    if (hidKey >= hid::KEY_LEFTCONTROL && hidKey <= hid::KEY_RIGHT_GUI) {
        const quint8 bit = 1 << (hidKey - hid::KEY_LEFTCONTROL);
        softKb_.ctrl &= ~bit;
    } else {
        int w = 0;
        for (int r = 0; r < 10; ++r) {
            if (softKb_.keys[r] != hidKey && softKb_.keys[r] != 0) {
                softKb_.keys[w++] = softKb_.keys[r];
            }
        }
        while (w < 10) softKb_.keys[w++] = 0;
    }
    return sendKeyboardCmd();
}

} // namespace kmbox
