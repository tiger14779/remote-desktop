#pragma once

// KMBox NET SDK 的 UDP 协议 —— 参考 https://github.com/kvmaibox/kmboxnet 的
// c++_demo/NetConfig/kmboxNet.{h,cpp}。协议是简单的 UDP 请求-响应:
//
//   头 16 字节: mac(4) rand(4) indexpts(4) cmd(4)     -- 全部小端
//   载荷:      soft_mouse_t(56B) 或 soft_keyboard_t(12B) 或空
//
// 与 SendInput / PostMessage 的本质差异: KMBox NET 是**独立 USB HID 硬件盒子**,
// UDP 命令过来后由盒子的 USB 芯片输出真实的键鼠 HID 报文给主机, Windows / 企微
// 看到的是"和一副真实键盘鼠标一样"的物理输入 —— 没有 LLKHF_INJECTED 标志,
// 也不用系统全局钩子, GetLastInputInfo 也会正常刷新。
//
// 本文件从 qt_WxSight/src/core/KmBoxNet.{h,cpp} 迁过来, 去掉了 Logger 依赖,
// 保留全部协议 / HID 键码 / 底层收发逻辑不变。

#include <QMutex>
#include <QString>
#include <cstdint>

namespace kmbox {

// HID Usage IDs (常用子集, 参见 USB HID Usage Tables §10 keyboard)。
// 注意: 这些**不是** Windows VK_ 常量, 而是标准 USB HID 键盘用法码。
namespace hid {
    constexpr int KEY_A            = 0x04;
    constexpr int KEY_Z            = 0x1D;
    constexpr int KEY_1            = 0x1E;
    constexpr int KEY_0            = 0x27;

    constexpr int KEY_ENTER        = 0x28;
    constexpr int KEY_ESCAPE       = 0x29;
    constexpr int KEY_BACKSPACE    = 0x2A;
    constexpr int KEY_TAB          = 0x2B;
    constexpr int KEY_SPACE        = 0x2C;
    constexpr int KEY_MINUS        = 0x2D;
    constexpr int KEY_EQUAL        = 0x2E;
    constexpr int KEY_LBRACKET     = 0x2F;
    constexpr int KEY_RBRACKET     = 0x30;
    constexpr int KEY_BACKSLASH    = 0x31;
    constexpr int KEY_SEMICOLON    = 0x33;
    constexpr int KEY_APOSTROPHE   = 0x34;
    constexpr int KEY_GRAVE        = 0x35;
    constexpr int KEY_COMMA        = 0x36;
    constexpr int KEY_PERIOD       = 0x37;
    constexpr int KEY_SLASH        = 0x38;
    constexpr int KEY_CAPSLOCK     = 0x39;

    constexpr int KEY_F1           = 0x3A;
    constexpr int KEY_F12          = 0x45;

    constexpr int KEY_PRINTSCREEN  = 0x46;
    constexpr int KEY_SCROLLLOCK   = 0x47;
    constexpr int KEY_PAUSE        = 0x48;
    constexpr int KEY_INSERT       = 0x49;
    constexpr int KEY_HOME         = 0x4A;
    constexpr int KEY_PAGEUP       = 0x4B;
    constexpr int KEY_DELETE       = 0x4C;
    constexpr int KEY_END          = 0x4D;
    constexpr int KEY_PAGEDOWN     = 0x4E;

    constexpr int KEY_RIGHT        = 0x4F;
    constexpr int KEY_LEFT         = 0x50;
    constexpr int KEY_DOWN         = 0x51;
    constexpr int KEY_UP           = 0x52;

    constexpr int KEY_NUMLOCK      = 0x53;
    constexpr int KEY_KP_DIVIDE    = 0x54;
    constexpr int KEY_KP_MULTIPLY  = 0x55;
    constexpr int KEY_KP_MINUS     = 0x56;
    constexpr int KEY_KP_PLUS      = 0x57;
    constexpr int KEY_KP_ENTER     = 0x58;
    constexpr int KEY_KP_1         = 0x59;
    constexpr int KEY_KP_0         = 0x62;
    constexpr int KEY_KP_DOT       = 0x63;

    // Modifier keys (SDK 里按 ctrl 位掩码单独处理)
    constexpr int KEY_LEFTCONTROL  = 0xE0;
    constexpr int KEY_LEFTSHIFT    = 0xE1;
    constexpr int KEY_LEFTALT      = 0xE2;
    constexpr int KEY_LEFT_GUI     = 0xE3;
    constexpr int KEY_RIGHTCONTROL = 0xE4;
    constexpr int KEY_RIGHTSHIFT   = 0xE5;
    constexpr int KEY_RIGHTALT     = 0xE6;
    constexpr int KEY_RIGHT_GUI    = 0xE7;
} // namespace hid

class KmBoxNet {
public:
    KmBoxNet();
    ~KmBoxNet();
    KmBoxNet(const KmBoxNet&) = delete;
    KmBoxNet& operator=(const KmBoxNet&) = delete;

    // uuidHex: LCD 上显示的 8 位 hex (如 "1F6E5054")
    // port:    LCD 上的端口 (如 443)
    bool init(const QString& ip, quint16 port, const QString& uuidHex,
              QString* err = nullptr);
    void close();
    bool isConnected() const { return connected_; }

    // ---- 鼠标 (相对当前光标位置) ----
    bool mouseMoveRel(short dx, short dy);
    // 拟人化平滑移动 (SDK cmd_mouse_automove, rand 字段传 ms)
    bool mouseMoveAuto(int dx, int dy, int ms);
    bool mouseLeft(bool down);
    bool mouseRight(bool down);
    bool mouseMiddle(bool down);
    bool mouseWheel(int wheel);

    // ---- 键盘 (HID 键码) ----
    bool keyDown(int hidKey);
    bool keyUp(int hidKey);

    // Cheap "are you still there?" probe. Re-sends CMD_CONNECT with no
    // payload — the box always ACKs it (it's the same command used by
    // init()). Returns false + err if the ACK doesn't land within the
    // socket's recv timeout, which is how the heartbeat monitor detects
    // a physically-unplugged box.
    bool ping(QString* err = nullptr);

private:
    // 发送一条命令并等响应 (3 秒超时)。thread-safe (内部持 mtx_)。
    bool sendCmd(quint32 cmd, quint32 randOrMs,
                 const void* payload, int payloadSize,
                 QString* err = nullptr);

    // 组一个 soft_mouse_t 载荷发出去
    bool sendMouseCmd(quint32 cmd, quint32 randOrMs,
                      int button, int x, int y, int wheel);
    // 组一个 soft_keyboard_t 载荷发出去 (用当前累积的 softKb_ 状态)
    bool sendKeyboardCmd();

    QMutex             mtx_;
    quintptr           sock_ = 0;
    void*              serverAddr_ = nullptr;
    QString            ip_;
    quint16            port_ = 0;
    quint32            uuidMac_ = 0;
    quint32            indexpts_ = 0;
    bool               connected_ = false;
    bool               wsaStarted_ = false;

    // C SDK 里的累积状态 —— button 是位掩码 (0x01=L 0x02=R 0x04=M ...);
    // keyboard 有 modifier 字节 + 最多 10 个同时按下的键。
    int softButton_ = 0;
    struct SoftKb {
        quint8 ctrl = 0;
        quint8 reserved = 0;
        quint8 keys[10] = {0,0,0,0,0,0,0,0,0,0};
    } softKb_;
};

} // namespace kmbox
