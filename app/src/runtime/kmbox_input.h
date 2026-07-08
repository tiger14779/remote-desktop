#pragma once

// Adapter that plugs a KmBoxNet instance into input::SetInputRouter.
//
// Purpose: when the operator wants to bypass WeChat Work's "someone remote-
// controlled you" detection, we route incoming viewer input frames through a
// physical USB HID device (KMBox NET) instead of Windows SendInput. Windows
// (and WeChat Work) sees the KMBox HID reports as a plain physical keyboard
// / mouse — no LLKHF_INJECTED flag, no hook chain, no detection.
//
// Not everything can be translated:
//   * Keys not in our VK→HID table (rare CJK layout stuff) — dropped with a
//     warning. Falling back to SendInput would defeat the whole point.
//   * Absolute mouse positioning is converted to a relative delta from
//     current cursor. Small drift is naturally corrected by the next
//     absolute frame the viewer sends.

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace kmbox { class KmBoxNet; }

class KmboxInputRouter : public QObject {
    Q_OBJECT
public:
    explicit KmboxInputRouter(QObject* parent = nullptr);
    ~KmboxInputRouter() override;

    // Try to connect to the KMBox box. Returns false + error text on failure.
    // Idempotent — safe to call to reconnect after a network hiccup.
    bool enable(const QString& ip, quint16 port, const QString& uuidHex,
                QString* err);
    // Disconnect and unregister the router from input:: so subsequent
    // frames fall back to SendInput.
    void disable();

    bool isEnabled() const { return enabled_.load(); }
    // Fast check the UI polls to decide whether to show the "hardware
    // active" or "hardware dead" indicator. True from a successful
    // enable() until the heartbeat monitor loses N consecutive pings.
    bool isBoxAlive() const { return boxAlive_.load(); }

signals:
    // Emitted on GUI thread when the router successfully translates a mouse
    // move — used by the UI to show "hardware input active" indicators. Not
    // fired for every frame (throttled to first move after enable, then
    // silent to avoid signal spam at 60 fps).
    void statusChanged(const QString& text);

    // Fires whenever the heartbeat monitor's alive/dead assessment flips.
    // On alive=false the UI raises a red warning and the router's
    // routeFrame() begins declining input (letting HandleInputFrame fall
    // back to SendInput — the user needs to know inputs are no longer
    // hardware-safe). On alive=true the router silently resumes routing.
    void boxAliveChanged(bool alive);

private:
    // Called from the network read thread via the input::InputRouter hook.
    // Returns true if the event was fully handled by KMBox (skip SendInput).
    bool routeFrame(std::uint8_t msgType, const std::uint8_t* data,
                    std::size_t size);

    std::unique_ptr<kmbox::KmBoxNet> box_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> boxAlive_{false};
    // Serialises access to box_ + softMods_. Router callback runs on the
    // network thread; enable()/disable() run on the GUI thread.
    mutable std::mutex mu_;

    // Heartbeat monitor. Every kHeartbeatIntervalMs ms, sends a KmBoxNet::
    // ping() and flips boxAlive_ + emits boxAliveChanged on transitions.
    // Threads:
    //   * heartbeatMu_ + heartbeatCv_: interruptible sleep so disable()
    //     doesn't have to wait a full interval before joining.
    std::thread              heartbeatThread_;
    std::mutex               heartbeatMu_;
    std::condition_variable  heartbeatCv_;
    std::atomic<bool>        heartbeatStop_{false};
};
