#include "ui/app_window.h"

#include "runtime/autostart.h"
#include "runtime/kmbox_input.h"
#include "runtime/log_sink.h"
#include "runtime/permission.h"

#include "net/net_prefs.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Viewer's MainWindow class lives at viewer/src/ui/main_window.h; that path
// is on our include search list via app/CMakeLists.
#include "ui/clipboard_file_xfer.h"
#include "ui/main_window.h"
#include "ui/viewer_session.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <thread>

#include <QProgressBar>

namespace {
// Small frameless top-most widget for the "正在被远程" alert. Broken out
// because we need mouse-drag handling on the widget itself (event filters
// on a bare QWidget stop working the moment the child QLabel eats the
// event) and per-app-lifetime position memory so a user who drags it to a
// less-obtrusive spot doesn't have to redo it on the next session.
//
// A second row shows an in-flight file/folder transfer's percent + a
// summary label. Hidden by default; grows the banner when it appears so a
// long path doesn't get squashed.
class SessionBannerWidget : public QWidget {
public:
    explicit SessionBannerWidget(const QString& deviceId, QWidget* parent,
                                  Qt::WindowFlags f)
        : QWidget(parent, f) {
        setObjectName(QStringLiteral("sessionBanner"));
        // Solid red-ish background + white text = the classic "警告 / 录制中"
        // colour scheme users already read as "something is watching".
        setStyleSheet(QStringLiteral(
            "QWidget#sessionBanner {"
            "  background-color: #d23428;"
            "  border: 2px solid #8a1e15;"
            "  border-radius: 8px;"
            "}"
            "QLabel { color: white; font-weight: bold; }"
            "QProgressBar {"
            "  background-color: #7a1e15;"
            "  border: 1px solid #4a0f0a;"
            "  border-radius: 4px;"
            "  color: white;"
            "  text-align: center;"
            "  min-height: 14px;"
            "}"
            "QProgressBar::chunk {"
            "  background-color: #f5f5f5;"
            "  border-radius: 3px;"
            "}"));
        // Window-level opacity: makes whatever is underneath still readable
        // through the alert. 0.82 is the sweet spot in practice.
        setWindowOpacity(0.82);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(14, 8, 14, 8);
        root->setSpacing(6);

        // Row 1: dot + main "being controlled" text.
        auto* row1 = new QHBoxLayout;
        row1->setSpacing(10);

        auto* dot = new QLabel(QStringLiteral("●"));
        QFont dotFont = dot->font();
        dotFont.setPointSize(dotFont.pointSize() + 4);
        dot->setFont(dotFont);
        dot->setStyleSheet(QStringLiteral("color: white;"));
        row1->addWidget(dot);

        auto* text = new QLabel(QStringLiteral("正在被远程控制 · 本机 ID %1"
                                                "   (可拖动)").arg(deviceId));
        QFont tf = text->font();
        tf.setPointSize(tf.pointSize() + 1);
        text->setFont(tf);
        row1->addWidget(text, 1);

        // Row 2 (hidden by default): file transfer status + action button.
        // The action button is the operator's ONLY way to actually push
        // clipboard files to the peer — clipboard changes are now
        // detection-only, no auto-send.
        auto* row2 = new QHBoxLayout;
        row2->setSpacing(8);
        xferLabel_ = new QLabel(QStringLiteral(""));
        xferLabel_->setStyleSheet(QStringLiteral("color: white; font-weight: normal;"));
        xferBar_ = new QProgressBar;
        xferBar_->setRange(0, 100);
        xferBar_->setValue(0);
        xferBar_->setTextVisible(true);
        xferBar_->setMinimumWidth(120);
        actionButton_ = new QPushButton(QStringLiteral("发送到对方"));
        actionButton_->setCursor(Qt::PointingHandCursor);
        actionButton_->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: #ffffff;"
            "  color: #8a1e15;"
            "  border: 1px solid #ffffff;"
            "  border-radius: 4px;"
            "  padding: 3px 10px;"
            "  font-weight: bold;"
            "}"
            "QPushButton:hover { background-color: #f5f5f5; }"
            "QPushButton:pressed { background-color: #d0d0d0; }"));
        row2->addWidget(xferLabel_, 1);
        row2->addWidget(xferBar_, 1);
        row2->addWidget(actionButton_);
        xferBar_->hide();
        xferLabel_->hide();
        actionButton_->hide();

        // Row 3 (hidden by default): KMBox hardware-dead warning. Yellow
        // text on the red banner background gives a "warning inside an
        // alert" look that reads as more urgent than either alone.
        kmboxWarnLabel_ = new QLabel(QStringLiteral(
            "⚠ 硬件模式已掉线, 输入已切回 SendInput (不安全 - 企业微信可能告警)"));
        kmboxWarnLabel_->setStyleSheet(QStringLiteral(
            "color: #ffde59; font-weight: bold; padding: 2px 0;"));
        kmboxWarnLabel_->hide();

        root->addLayout(row1);
        root->addLayout(row2);
        root->addWidget(kmboxWarnLabel_);

        // Prevent child widgets from swallowing mouse events before our
        // drag handler sees them. TransparentForMouseEvents on the labels
        // makes clicks fall through to the SessionBannerWidget parent.
        // Button KEEPS mouse events (it's the whole point) — dragging by
        // the button just doesn't work, drag from anywhere else does.
        dot->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        xferLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        xferBar_->setAttribute(Qt::WA_TransparentForMouseEvents);
        kmboxWarnLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);

        QObject::connect(actionButton_, &QPushButton::clicked, this, [this]() {
            if (onActionClicked_) onActionClicked_();
        });
    }

    // True after the user has dragged us at least once.
    bool hasBeenDragged() const { return dragged_; }

    // Show the transfer row with a summary line + percentage bar. Called
    // when ClipboardFileXfer starts sending or receiving.
    void beginTransfer(bool sending, qint64 totalBytes, int fileCount) {
        const QString dir = sending ? QStringLiteral("发送")
                                    : QStringLiteral("接收");
        xferLabel_->setText(QStringLiteral("%1 %2 个文件, 共 %3 KB")
                                .arg(dir).arg(fileCount).arg(totalBytes / 1024));
        xferBar_->setValue(0);
        xferLabel_->show();
        xferBar_->show();
        adjustSize();
    }

    // Update the running percentage (0–100).
    void updateTransfer(bool sending, qint64 doneBytes, qint64 totalBytes,
                        int percent) {
        const QString dir = sending ? QStringLiteral("发送")
                                    : QStringLiteral("接收");
        xferLabel_->setText(QStringLiteral("%1 中: %2 / %3 KB")
                                .arg(dir)
                                .arg(doneBytes / 1024)
                                .arg(totalBytes / 1024));
        xferBar_->setValue(percent);
    }

    // Hide the transfer row after a transfer finishes. Small delay handled
    // by caller if it wants to flash "完成" before hiding.
    void endTransfer() {
        xferLabel_->hide();
        xferBar_->hide();
        adjustSize();
    }

    // Show the "→ 发送 N 项到对方" call-to-action. Called by AppMainWindow
    // when local clipboard picks up file URLs while a session is active.
    void showSendPrompt(int topLevelCount) {
        actionButton_->setText(QStringLiteral("→ 发送 %1 项到对方")
                                    .arg(topLevelCount));
        actionButton_->show();
        adjustSize();
    }
    // Swap the button to "取消传输" mode. Same button, same click
    // callback — AppMainWindow's callback dispatches on state.
    void showCancelPrompt() {
        actionButton_->setText(QStringLiteral("取消传输"));
        actionButton_->show();
        adjustSize();
    }
    void hideActionButton() {
        actionButton_->hide();
        adjustSize();
    }

    // KMBox heartbeat monitor calls these when the box goes offline /
    // reconnects while the operator has hardware mode enabled.
    void showKmboxWarning() {
        kmboxWarnLabel_->show();
        adjustSize();
    }
    void hideKmboxWarning() {
        kmboxWarnLabel_->hide();
        adjustSize();
    }

    // Callback fired when the action button is clicked. Set once by
    // AppMainWindow; the callback itself inspects state and dispatches
    // between sendClipboardNow / cancelActiveTransfer.
    void setActionCallback(std::function<void()> cb) {
        onActionClicked_ = std::move(cb);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            // Store the click offset within the widget so subsequent moves
            // translate global cursor position into a matching window
            // top-left. globalPosition() (double-precision Qt6) is what we
            // want here — screenPos() is deprecated.
            dragOffset_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging_ && (e->buttons() & Qt::LeftButton)) {
            move(e->globalPosition().toPoint() - dragOffset_);
            dragged_ = true;
            e->accept();
            return;
        }
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            setCursor(Qt::OpenHandCursor);
            e->accept();
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }
    void enterEvent(QEnterEvent* e) override {
        setCursor(Qt::OpenHandCursor);
        QWidget::enterEvent(e);
    }
    void leaveEvent(QEvent* e) override {
        unsetCursor();
        QWidget::leaveEvent(e);
    }

private:
    QLabel*       xferLabel_      = nullptr;
    QProgressBar* xferBar_        = nullptr;
    QPushButton*  actionButton_   = nullptr;
    QLabel*       kmboxWarnLabel_ = nullptr;
    std::function<void()> onActionClicked_;
    QPoint dragOffset_;
    bool   dragging_ = false;
    bool   dragged_  = false;
};
} // namespace

// Hardcoded relay endpoint — change here for now, will be configurable later.
static constexpr const char* kRelayHost = "yuancheng.skopn.co";
static constexpr int         kRelayPort = 7100;

// For the MVP we keep the relay's existing auth model (admin / session_token
// out of config.production.json). Future iteration extends CONNECT to carry
// per-target verifyCode.
static constexpr const char* kAccount      = "admin";
static constexpr const char* kSessionToken = "e009aed4ad56e47950d6c9c7d8b81eac40105b4d5d2b3938";
static constexpr const char* kDeviceToken  = "7f4ed9297289909c86fd83155e9ca992590898b962d3b6b0";

// Pull viewer's MainWindow class in. We typedef it to a non-clashing name
// so our AppMainWindow can coexist with it in the same translation unit.
#include "ui/main_window.h"   // this resolves to viewer/src/ui/main_window.h
                              // because viewer/src/ is on the include path AFTER app/src/
                              // (CMake adds both; later one wins for "ui/main_window.h")
// — actually both #include resolve to the SAME file when paths overlap. To
// avoid the collision we use a different path for the viewer's main window:
// it lives under viewer/src/ui/main_window.h. To disambiguate at file level
// we use the fully-qualified header path.

AppMainWindow::AppMainWindow(const LocalConfig& localCfg, QWidget* parent)
    : QMainWindow(parent), local_(localCfg) {
    setWindowTitle(QStringLiteral("Remote Desktop"));
    // Per-window icon in addition to QApplication::setWindowIcon — some
    // Windows versions read the QMainWindow one for the title-bar corner.
    setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setSpacing(12);
    root->setContentsMargins(16, 16, 16, 16);

    // === Local section: "this machine" ====================================
    {
        auto* g = new QGroupBox(QStringLiteral("本机 (别人可以连到我)"));
        auto* gl = new QVBoxLayout(g);

        auto* idRow = new QHBoxLayout;
        idRow->addWidget(new QLabel(QStringLiteral("ID:")));
        localIdField_ = new QLineEdit(local_.deviceId);
        localIdField_->setReadOnly(true);
        QFont mono = localIdField_->font();
        mono.setFamily(QStringLiteral("Consolas"));
        mono.setPointSize(mono.pointSize() + 4);
        localIdField_->setFont(mono);
        idRow->addWidget(localIdField_, 1);
        auto* copyId = new QPushButton(QStringLiteral("复制"));
        idRow->addWidget(copyId);
        connect(copyId, &QPushButton::clicked, this, [this]() {
            copyToClipboard(localIdField_);
        });
        gl->addLayout(idRow);

        statusLabel_ = new QLabel(QStringLiteral("启动中..."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #666;"));
        gl->addWidget(statusLabel_);

        // Auto-start on Windows logon. Toggling writes/removes a value under
        // HKCU\Software\Microsoft\Windows\CurrentVersion\Run — no admin
        // prompt because it's per-user.
        autoStartBox_ = new QCheckBox(QStringLiteral("开机自启 (登录 Windows 时自动运行)"));
        autoStartBox_->setChecked(Autostart::isEnabled());
        connect(autoStartBox_, &QCheckBox::toggled, this, [this](bool on) {
            const bool ok = on ? Autostart::enable() : Autostart::disable();
            if (!ok) {
                QMessageBox::warning(
                    this, QStringLiteral("提示"),
                    QStringLiteral("无法修改开机自启注册表项。请检查权限。"));
                // Roll the visual state back to what actually persisted so
                // the box never lies about the underlying registry.
                QSignalBlocker block(autoStartBox_);
                autoStartBox_->setChecked(Autostart::isEnabled());
                appendLog(QStringLiteral("[autostart] 修改失败"));
                return;
            }
            appendLog(on ? QStringLiteral("[autostart] 已启用")
                        : QStringLiteral("[autostart] 已禁用"));
        });
        gl->addWidget(autoStartBox_);

        root->addWidget(g);
    }

    // === KMBox NET hardware input backend =================================
    // Routes viewer key/mouse frames through a physical USB HID box instead
    // of Windows SendInput. Rationale in kmbox_input.h.
    {
        auto* g = new QGroupBox(QStringLiteral("硬件键鼠 (KMBox NET, 防止企业微信告警)"));
        auto* gl = new QVBoxLayout(g);

        auto* row1 = new QHBoxLayout;
        row1->addWidget(new QLabel(QStringLiteral("盒子 IP:")));
        kmboxIpField_ = new QLineEdit(local_.kmboxIp);
        kmboxIpField_->setPlaceholderText(QStringLiteral("192.168.1.230"));
        kmboxIpField_->setMaximumWidth(160);
        row1->addWidget(kmboxIpField_);
        row1->addWidget(new QLabel(QStringLiteral("端口:")));
        kmboxPortField_ = new QLineEdit(
            local_.kmboxPort ? QString::number(local_.kmboxPort) : QString());
        kmboxPortField_->setPlaceholderText(QStringLiteral("443"));
        kmboxPortField_->setMaximumWidth(70);
        row1->addWidget(kmboxPortField_);
        row1->addWidget(new QLabel(QStringLiteral("UUID:")));
        kmboxUuidField_ = new QLineEdit(local_.kmboxUuid);
        kmboxUuidField_->setPlaceholderText(QStringLiteral("1F6E5054"));
        kmboxUuidField_->setMaxLength(8);
        kmboxUuidField_->setMaximumWidth(90);
        row1->addWidget(kmboxUuidField_);
        row1->addStretch(1);
        gl->addLayout(row1);

        kmboxEnableBox_ = new QCheckBox(
            QStringLiteral("启用硬件键鼠 (勾选后被控端键鼠输入走盒子, "
                           "SendInput 不再触发, 企业微信不会告警)"));
        gl->addWidget(kmboxEnableBox_);

        kmboxStatusLabel_ = new QLabel(QStringLiteral("未连接"));
        kmboxStatusLabel_->setStyleSheet(QStringLiteral("color: #888;"));
        gl->addWidget(kmboxStatusLabel_);

        // Router lives for the app's lifetime — we just toggle its
        // enable/disable state and re-init on config edit.
        kmboxRouter_ = new KmboxInputRouter(this);
        // QueuedConnection because enable()/disable() (which emit this) can
        // run on the async connect thread — touching QLabel there would be
        // a Qt threading violation.
        connect(kmboxRouter_, &KmboxInputRouter::statusChanged, this,
                [this](const QString& s) {
                    kmboxStatusLabel_->setText(s);
                    appendLog(QStringLiteral("[kmbox] %1").arg(s));
                }, Qt::QueuedConnection);
        // Heartbeat verdict — flips the banner warning and the main
        // window's status label. Emitted from the heartbeat thread, so
        // QueuedConnection is mandatory.
        connect(kmboxRouter_, &KmboxInputRouter::boxAliveChanged, this,
                [this](bool alive) {
                    if (alive) {
                        appendLog(QStringLiteral(
                            "[kmbox] 硬件恢复, 输入重新走盒子"));
                        kmboxStatusLabel_->setText(QStringLiteral(
                            "✓ 已连接 %1:%2 (键鼠走硬件, SendInput 已停用)")
                            .arg(local_.kmboxIp).arg(local_.kmboxPort));
                        kmboxStatusLabel_->setStyleSheet(QStringLiteral(
                            "color: #2a7a2a; font-weight: bold;"));
                        if (auto* b = static_cast<SessionBannerWidget*>(
                                sessionBanner_)) {
                            b->hideKmboxWarning();
                        }
                    } else {
                        appendLog(QStringLiteral(
                            "[kmbox] ⚠ 心跳丢失, 输入已切回 SendInput "
                            "(企业微信可能告警)"));
                        kmboxStatusLabel_->setText(QStringLiteral(
                            "⚠ 心跳丢失, 输入已切回 SendInput"));
                        kmboxStatusLabel_->setStyleSheet(QStringLiteral(
                            "color: #c33; font-weight: bold;"));
                        if (auto* b = static_cast<SessionBannerWidget*>(
                                sessionBanner_)) {
                            b->showKmboxWarning();
                        }
                    }
                }, Qt::QueuedConnection);

        auto persistFields = [this]() {
            local_.kmboxIp   = kmboxIpField_->text().trimmed();
            local_.kmboxPort = static_cast<quint16>(
                kmboxPortField_->text().trimmed().toUInt());
            local_.kmboxUuid = kmboxUuidField_->text().trimmed();
            SaveLocalConfig(local_);
        };
        // Debounced-ish: persist on editingFinished (Enter or focus loss),
        // NOT on every keystroke. Users typing an IP shouldn't cause 12
        // disk writes.
        connect(kmboxIpField_,   &QLineEdit::editingFinished, this, persistFields);
        connect(kmboxPortField_, &QLineEdit::editingFinished, this, persistFields);
        connect(kmboxUuidField_, &QLineEdit::editingFinished, this, persistFields);

        // Async connect helper. init() calls recvfrom() with a 3s timeout,
        // which would freeze the GUI (and the entire window construction
        // path on startup) if run on the main thread. We spawn a detached
        // std::thread, then hop back with QMetaObject::invokeMethod to
        // update the status label. Kept as a captureable lambda so both
        // the checkbox handler and the startup auto-restore call it.
        auto asyncConnect = [this](const QString& ip, quint16 port,
                                    const QString& uuid,
                                    bool interactive) {
            kmboxStatusLabel_->setText(QStringLiteral("正在连接 %1:%2 ...")
                                            .arg(ip).arg(port));
            kmboxStatusLabel_->setStyleSheet(QStringLiteral("color: #888;"));
            std::thread([this, ip, port, uuid, interactive]() {
                QString err;
                const bool ok = kmboxRouter_->enable(ip, port, uuid, &err);
                QMetaObject::invokeMethod(this, [this, ok, err, ip, port,
                                                  interactive]() {
                    if (ok) {
                        kmboxStatusLabel_->setText(QStringLiteral(
                            "✓ 已连接 %1:%2 (键鼠走硬件, SendInput 已停用)")
                            .arg(ip).arg(port));
                        kmboxStatusLabel_->setStyleSheet(
                            QStringLiteral("color: #2a7a2a; font-weight: bold;"));
                    } else {
                        // Interactive = user just clicked the checkbox
                        // and expects a modal answer. Startup auto-connect
                        // failures are silent (log + status label only)
                        // so a temporarily offline box doesn't nag the
                        // user every launch.
                        if (interactive) {
                            QMessageBox::warning(
                                this, QStringLiteral("kmbox 连接失败"), err);
                            QSignalBlocker block(kmboxEnableBox_);
                            kmboxEnableBox_->setChecked(false);
                            local_.kmboxEnabled = false;
                            SaveLocalConfig(local_);
                        } else {
                            appendLog(QStringLiteral("[kmbox] 自动连接失败: %1")
                                          .arg(err));
                        }
                        kmboxStatusLabel_->setText(
                            QStringLiteral("连接失败: %1").arg(err));
                        kmboxStatusLabel_->setStyleSheet(
                            QStringLiteral("color: #c33;"));
                    }
                }, Qt::QueuedConnection);
            }).detach();
        };

        auto applyEnable = [this, asyncConnect](bool on) {
            local_.kmboxEnabled = on;
            SaveLocalConfig(local_);
            if (on) {
                const QString ip = kmboxIpField_->text().trimmed();
                const quint16 port = static_cast<quint16>(
                    kmboxPortField_->text().trimmed().toUInt());
                const QString uuid = kmboxUuidField_->text().trimmed();
                if (ip.isEmpty() || port == 0 || uuid.size() != 8) {
                    QMessageBox::warning(
                        this, QStringLiteral("提示"),
                        QStringLiteral("请填写 IP / 端口 / 8 位 UUID 后再启用"));
                    QSignalBlocker block(kmboxEnableBox_);
                    kmboxEnableBox_->setChecked(false);
                    local_.kmboxEnabled = false;
                    SaveLocalConfig(local_);
                    return;
                }
                asyncConnect(ip, port, uuid, /*interactive=*/true);
            } else {
                kmboxRouter_->disable();
                kmboxStatusLabel_->setText(QStringLiteral("未连接"));
                kmboxStatusLabel_->setStyleSheet(QStringLiteral("color: #888;"));
            }
        };
        connect(kmboxEnableBox_, &QCheckBox::toggled, this, applyEnable);

        // Restore prior state: if the user had enable ticked last run and
        // the settings are complete, kick off a background connect after
        // the event loop starts. Blocking here would spin the launch
        // cursor for 3 seconds (or longer) when the box is off — that's
        // what was locking the user out of the app entirely.
        if (local_.kmboxEnabled &&
            !local_.kmboxIp.isEmpty() && local_.kmboxPort > 0 &&
            local_.kmboxUuid.size() == 8) {
            QSignalBlocker block(kmboxEnableBox_);
            kmboxEnableBox_->setChecked(true);
            const QString ip   = local_.kmboxIp;
            const quint16 port = local_.kmboxPort;
            const QString uuid = local_.kmboxUuid;
            // singleShot(0) defers to the first event loop tick — window
            // is fully constructed and visible by then.
            QTimer::singleShot(0, this, [this, ip, port, uuid, asyncConnect]() {
                asyncConnect(ip, port, uuid, /*interactive=*/false);
            });
        }

        root->addWidget(g);
    }

    // === Remote section: "connect to" ======================================
    {
        auto* g = new QGroupBox(QStringLiteral("连接对方"));
        auto* gl = new QVBoxLayout(g);

        auto* idRow = new QHBoxLayout;
        idRow->addWidget(new QLabel(QStringLiteral("远程 ID:")));
        remoteIdField_ = new QLineEdit;
        remoteIdField_->setPlaceholderText(QStringLiteral("对方的 8 位 ID"));
        // Enter inside the field acts the same as clicking 连接, so the
        // typical "type id, hit enter" flow doesn't need to find the mouse.
        connect(remoteIdField_, &QLineEdit::returnPressed,
                this, &AppMainWindow::onConnectClicked);
        idRow->addWidget(remoteIdField_, 1);
        gl->addLayout(idRow);

        connectButton_ = new QPushButton(QStringLiteral("连接"));
        connectButton_->setMinimumHeight(36);
        QFont bigger = connectButton_->font();
        bigger.setPointSize(bigger.pointSize() + 2);
        bigger.setBold(true);
        connectButton_->setFont(bigger);
        connect(connectButton_, &QPushButton::clicked, this, &AppMainWindow::onConnectClicked);
        gl->addWidget(connectButton_);

        // Recent IDs — populated from LocalConfig::recentRemoteIds, persisted
        // across launches. Double-click connects immediately so the common
        // "I want back into the box I had open earlier" path stays one
        // gesture. Single-click only fills the input field (gives the
        // operator a chance to bail out if it's the wrong machine).
        gl->addWidget(new QLabel(QStringLiteral("最近连接:")));
        recentList_ = new QListWidget;
        recentList_->setMaximumHeight(120);
        connect(recentList_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* item) {
                    if (item) remoteIdField_->setText(item->text());
                });
        connect(recentList_, &QListWidget::itemActivated,
                this, &AppMainWindow::onRecentItemActivated);
        gl->addWidget(recentList_);

        root->addWidget(g);
    }

    // === Log section =====================================================
    {
        auto* g = new QGroupBox(QStringLiteral("日志 (可复制发给作者排查问题)"));
        auto* gl = new QVBoxLayout(g);

        logView_ = new QPlainTextEdit;
        logView_->setReadOnly(true);
        logView_->setMaximumBlockCount(5000); // cap memory; old lines drop off
        logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(mono.pointSize() - 1);
        logView_->setFont(mono);
        logView_->setMinimumHeight(180);
        gl->addWidget(logView_, 1);

        auto* btnRow = new QHBoxLayout;
        btnRow->addStretch(1);
        auto* copyBtn = new QPushButton(QStringLiteral("复制全部"));
        auto* clearBtn = new QPushButton(QStringLiteral("清空"));
        btnRow->addWidget(copyBtn);
        btnRow->addWidget(clearBtn);
        gl->addLayout(btnRow);

        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            QApplication::clipboard()->setText(logView_->toPlainText());
        });
        connect(clearBtn, &QPushButton::clicked, logView_, &QPlainTextEdit::clear);

        root->addWidget(g, 1);  // log section takes any extra vertical space
    }

    setCentralWidget(central);
    setMinimumSize(480, 640);

    // Subscribe to the global log sink (installed in main() before us).
    // Anything anyone writes to std::cerr or std::cout ends up here, plus
    // any structured status updates from AgentRunner.
    connect(LogSink::instance(), &LogSink::line,
            this, &AppMainWindow::onLogLine, Qt::QueuedConnection);

    // Clipboard sync: when a viewer is attached to us as agent, mirror text
    // both directions. Loop-break is identical to the viewer side — track
    // last-net-text, ignore self-induced dataChanged.
    connect(&agent_, &AgentRunner::clipboardReceived,
            this, &AppMainWindow::onAgentClipboardReceived, Qt::QueuedConnection);
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
            this, &AppMainWindow::onLocalClipboardChanged);

    // Clipboard file transfer (agent-side participant). Same QClipboard
    // subscription drives both text and file flows; ClipboardFileXfer only
    // engages when mimeData has URLs.
    fileXfer_ = new ClipboardFileXfer(this);
    fileXfer_->setSendFn([this](std::uint8_t t, const std::uint8_t* d, std::size_t s) {
        agent_.sendFrameRaw(
            t, QByteArray(reinterpret_cast<const char*>(d), static_cast<int>(s)));
    });
    connect(&agent_, &AgentRunner::fileXferFrameReceived,
            this, [this](std::uint8_t t, const QByteArray& p) {
                if (fileXfer_) {
                    fileXfer_->onPeerFrame(
                        t, reinterpret_cast<const std::uint8_t*>(p.constData()),
                        static_cast<std::size_t>(p.size()));
                }
            }, Qt::QueuedConnection);
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
            this, [this]() { if (fileXfer_) fileXfer_->onLocalClipboardChanged(); });
    connect(fileXfer_, &ClipboardFileXfer::log,
            this, &AppMainWindow::appendLog);
    // Live transfer status on the "正在被远程" banner. QueuedConnection
    // because xferProgress fires from ClipboardFileXfer's send worker
    // thread — we must hop back to the GUI thread before touching QWidget.
    connect(fileXfer_, &ClipboardFileXfer::xferStarted, this,
            [this](bool sending, qint64 total, int fileCount) {
                xferInFlight_ = true;
                if (auto* b = static_cast<SessionBannerWidget*>(sessionBanner_)) {
                    b->beginTransfer(sending, total, fileCount);
                }
                refreshBannerAction();
            }, Qt::QueuedConnection);
    connect(fileXfer_, &ClipboardFileXfer::xferProgress, this,
            [this](bool sending, qint64 done, qint64 total, int pct) {
                if (auto* b = static_cast<SessionBannerWidget*>(sessionBanner_)) {
                    b->updateTransfer(sending, done, total, pct);
                }
            }, Qt::QueuedConnection);
    connect(fileXfer_, &ClipboardFileXfer::xferFinished, this,
            [this](bool, bool, qint64) {
                xferInFlight_ = false;
                if (auto* b = static_cast<SessionBannerWidget*>(sessionBanner_)) {
                    b->endTransfer();
                }
                refreshBannerAction();
            }, Qt::QueuedConnection);
    // Local clipboard now has (or lost) sendable file URLs. Detection-only;
    // actual send waits for the operator to click the banner button.
    connect(fileXfer_, &ClipboardFileXfer::localFilesAvailable, this,
            [this](bool available, int topLevelCount) {
                clipboardHasSendable_ = available;
                clipboardTopLevelCount_ = topLevelCount;
                refreshBannerAction();
            });

    // Spin up the always-on agent connection.
    AgentRunner::Config rcfg;
    rcfg.relayHost = QString::fromLatin1(kRelayHost);
    rcfg.relayPort = kRelayPort;
    rcfg.deviceId = local_.deviceId;
    // For now reuse the master device token. Future: per-device dynamic
    // registration where the relay accepts an arbitrary deviceId+token.
    rcfg.deviceToken = QString::fromLatin1(kDeviceToken);
    // 30 fps target + 4 Mbps for the productivity workflow. Once the
    // decoder's low-delay path is on (see H264Decoder::Init), the biggest
    // remaining piece of latency is the frame-period wait (33ms at 30fps)
    // and that is small enough for typing / clicking / occasional window
    // drag. The old 120fps setting was fighting hidden decoder buffering,
    // which is gone now — extra fps just eats bandwidth without perceptible
    // improvement in productivity workloads. Bitrate scaled down to match.
    rcfg.fps = 30;
    rcfg.bitrateKbps = 4000;
    rcfg.maxHeight = 1440;
    connect(&agent_, &AgentRunner::status, this,
            &AppMainWindow::onAgentStatus, Qt::QueuedConnection);
    connect(&agent_, &AgentRunner::sessionStarted, this,
            &AppMainWindow::onSessionStarted, Qt::QueuedConnection);
    connect(&agent_, &AgentRunner::sessionEnded, this,
            &AppMainWindow::onSessionEnded, Qt::QueuedConnection);

    appendLog(QStringLiteral("app started — local id=%1").arg(local_.deviceId));
    refreshRecentList();
    // Seed the shared force-TCP flag from persisted config BEFORE agent
    // starts — otherwise the first UDP_INFO round-trip on session start
    // wouldn't see it.
    net_prefs::SetForceTcpVideo(local_.forceTcpVideo);
    if (local_.forceTcpVideo) {
        appendLog(QStringLiteral(
            "[net] 强制 TCP 视频路径已启用 (调试用) — 若网络恢复请在托盘菜单关闭"));
    }
    agent_.start(rcfg);

    setupTrayIcon();
}

void AppMainWindow::setupTrayIcon() {
    // Note: we deliberately do NOT gate on QSystemTrayIcon::isSystemTrayAvailable().
    // On Windows 11 that call can return false at startup (the shell tray
    // is still spinning up) but tray icons work fine once we actually
    // ->show() them. Trust show() and check afterwards.
    trayIconIdle_ = QIcon(QStringLiteral(":/app_icon.png"));
    if (trayIconIdle_.isNull()) {
        appendLog(QStringLiteral("[tray] app_icon.png resource is empty — tray icon will be blank"));
    }
    // "Being controlled" variant: same icon with a small red badge painted
    // over the bottom-right — cheap visual signal in the tray area that a
    // session is active. Built by rendering the base pixmap once and
    // stamping a solid red circle in the corner.
    {
        QPixmap base = trayIconIdle_.pixmap(64, 64);
        if (!base.isNull()) {
            QPixmap withBadge = base;
            QPainter p(&withBadge);
            p.setRenderHint(QPainter::Antialiasing, true);
            const int d = base.width() / 3;                  // badge diameter
            const int x = base.width() - d - 2;
            const int y = base.height() - d - 2;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 40, 40));
            p.drawEllipse(x, y, d, d);
            p.end();
            trayIconActive_ = QIcon(withBadge);
        }
    }
    const QIcon& icon = trayIconIdle_;

    trayMenu_ = new QMenu(this);
    auto* showAct = trayMenu_->addAction(QStringLiteral("显示主窗口"));
    connect(showAct, &QAction::triggered, this, [this]() {
        activateFromBackground();
    });
    trayMenu_->addSeparator();
    // Diagnostic toggle. Common failure mode: customer's UDP path drops
    // fragmented IDR packets → viewer black screen. Flipping this to ON
    // proves whether the problem is UDP-specific (video appears immediately
    // over TCP) vs a real code bug (still black). Only takes effect on the
    // next session, not the currently active one.
    auto* forceTcpAct = trayMenu_->addAction(QStringLiteral("强制 TCP 视频 (调试)"));
    forceTcpAct->setCheckable(true);
    forceTcpAct->setChecked(local_.forceTcpVideo);
    connect(forceTcpAct, &QAction::toggled, this, [this](bool on) {
        net_prefs::SetForceTcpVideo(on);
        local_.forceTcpVideo = on;
        SaveLocalConfig(local_);
        appendLog(on ? QStringLiteral(
                           "[net] 强制 TCP 视频: 开 — 下次会话自动切窄带宽档 "
                           "(15fps / 1.5Mbps / 720p),需断开重连才生效")
                     : QStringLiteral(
                           "[net] 强制 TCP 视频: 关 — 下次会话恢复 UDP 与全档"));
        if (tray_) {
            tray_->showMessage(
                QStringLiteral("Remote Desktop"),
                on ? QStringLiteral("已切到 TCP + 窄带宽档,请断开重连生效")
                   : QStringLiteral("已恢复 UDP 高帧率档,请断开重连生效"),
                QSystemTrayIcon::Information, 3000);
        }
    });
    trayMenu_->addSeparator();
    auto* quitAct = trayMenu_->addAction(QStringLiteral("退出"));
    connect(quitAct, &QAction::triggered, this, [this]() {
        // Real quit path: bypass the close-to-tray trick in closeEvent.
        reallyQuit_ = true;
        agent_.stop();
        QApplication::quit();
    });

    tray_ = new QSystemTrayIcon(icon, this);
    tray_->setToolTip(QStringLiteral("Remote Desktop — 本机 ID %1").arg(local_.deviceId));
    tray_->setContextMenu(trayMenu_);
    // Left-click / double-click toggles the main window — matches the
    // WeChat / RustDesk convention.
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::Trigger ||
                    r == QSystemTrayIcon::DoubleClick) {
                    if (isVisible() && !isMinimized()) {
                        hide();
                    } else {
                        activateFromBackground();
                    }
                }
            });
    tray_->show();
    appendLog(QStringLiteral("[tray] created, visible=%1, sys_available=%2")
                  .arg(tray_->isVisible() ? "yes" : "no")
                  .arg(QSystemTrayIcon::isSystemTrayAvailable() ? "yes" : "no"));
    // Windows 11 hides all non-Microsoft tray icons in the overflow flyout
    // by default. Warn the operator so they know to check the "^" chevron
    // if they can't find the icon.
    if (tray_->isVisible()) {
        appendLog(QStringLiteral(
            "[tray] Win11 提示: 若通知区看不到图标, 请点任务栏 \"^\" 展开隐藏图标, "
            "或右键任务栏 -> 任务栏设置 -> 其他系统托盘图标, 把 Remote Desktop 打开"));
    }
}

AppMainWindow::~AppMainWindow() {
    // sessionBanner_ is a top-level widget with no parent — Qt won't clean
    // it up when the main window dies, so do it explicitly.
    if (sessionBanner_) {
        sessionBanner_->deleteLater();
        sessionBanner_ = nullptr;
    }
}

void AppMainWindow::activateFromBackground() {
    // Un-minimise while preserving max/full-screen if we happened to be
    // in one of those states. Doing state clearing before show() means the
    // window map animation shows the correct final size.
    if (isMinimized()) {
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    }
    show();
    raise();
    activateWindow();

#ifdef _WIN32
    // Windows foreground-lock workaround. show()/raise()/activateWindow()
    // paints the window and makes it z-order top, but SetForegroundWindow
    // is subject to a lock that silently no-ops when the caller isn't the
    // current foreground process — which we typically aren't (we came
    // from either a tray click or a QLocalServer connection from a
    // secondary launcher process). The workaround is to briefly attach
    // our input queue to whichever thread OWNS the current foreground
    // window: once attached, SetForegroundWindow treats us as "same input
    // queue as the current fg" and grants the call. Detach immediately
    // after so we don't leak the input-queue coupling.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    HWND fg = ::GetForegroundWindow();
    if (fg && fg != hwnd) {
        DWORD fgTid = ::GetWindowThreadProcessId(fg, nullptr);
        DWORD myTid = ::GetCurrentThreadId();
        if (fgTid != 0 && fgTid != myTid) {
            ::AttachThreadInput(fgTid, myTid, TRUE);
            ::SetForegroundWindow(hwnd);
            ::BringWindowToTop(hwnd);
            ::AttachThreadInput(fgTid, myTid, FALSE);
        } else {
            ::SetForegroundWindow(hwnd);
            ::BringWindowToTop(hwnd);
        }
    }
#endif
}

void AppMainWindow::closeEvent(QCloseEvent* e) {
    // Close-to-tray: keep the agent connection alive so the machine remains
    // reachable even after the operator dismisses the window. Only actually
    // shut down when the tray menu's "退出" is used (reallyQuit_ set).
    //
    // We deliberately do NOT gate on tray_->isVisible() here — right after
    // ->show() Windows may still report the icon as not-yet-shown (esp. on
    // Win11 with icons tucked into the overflow flyout), and we'd rather
    // stay resident than surprise-quit the user's agent session.
    if (!reallyQuit_ && tray_) {
        static bool notified = false;
        if (!notified && tray_->isSystemTrayAvailable()) {
            tray_->showMessage(QStringLiteral("Remote Desktop"),
                               QStringLiteral("已最小化到系统托盘，本机仍可被远程连接。右键托盘图标可退出。"),
                               QSystemTrayIcon::Information, 3000);
            notified = true;
        }
        hide();
        e->ignore();
        return;
    }
    // Fallback path: either the user chose the tray menu's "退出" (reallyQuit_
    // set) OR tray creation failed entirely. Since we set
    // QApplication::setQuitOnLastWindowClosed(false) in main(), we have to
    // explicitly ask the event loop to quit — otherwise the process would
    // just hang with no windows and no way out.
    hideSessionBanner();
    agent_.stop();
    e->accept();
    QApplication::quit();
}

void AppMainWindow::copyToClipboard(QLineEdit* src) {
    QApplication::clipboard()->setText(src->text());
}

void AppMainWindow::onAgentStatus(const QString& s) {
    statusLabel_->setText(s);
    appendLog(QStringLiteral("[agent] ") + s);
}

void AppMainWindow::onLogLine(const QString& s) {
    appendLog(s);
}

void AppMainWindow::appendLog(const QString& s) {
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logView_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, s));
    // Auto-scroll to bottom so the latest line is always visible.
    QScrollBar* sb = logView_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void AppMainWindow::onAgentClipboardReceived(const QString& text) {
    if (text == lastNetText_) return;
    lastNetText_ = text;
    QApplication::clipboard()->setText(text);
}

void AppMainWindow::onLocalClipboardChanged() {
    const QString text = QApplication::clipboard()->text();
    if (text == lastNetText_) return;  // echo from our own setText
    if (text.isEmpty()) return;
    lastNetText_ = text;
    agent_.sendClipboardText(text);
}

void AppMainWindow::onConnectClicked() {
    // Hidden capability gate — the app can always ACCEPT a remote
    // connection, but INITIATING one requires the super-password file
    // sitting next to the exe. Deliberately vague message so operators
    // without the file don't get a hint about the mechanism.
    if (!permission::HasControlPermission()) {
        QMessageBox::warning(this, QStringLiteral("缺少权限"),
                             QStringLiteral("当前安装未授权远程控制他人。\n"
                                            "请联系管理员。"));
        return;
    }
    const QString remoteId = remoteIdField_->text().trimmed();
    if (remoteId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请输入远程 ID"));
        return;
    }
    appendLog(QStringLiteral("connecting to remote id=%1").arg(remoteId));
    rememberRecent(remoteId);

    // Spawn a separate viewer window. We reuse viewer's existing MainWindow
    // class (which is the full video display + input capture window). It
    // takes a ViewerSession::Config and runs to completion.
    ViewerSession::Config vcfg;
    vcfg.host = QString::fromLatin1(kRelayHost);
    vcfg.port = kRelayPort;
    vcfg.account = QString::fromLatin1(kAccount);
    vcfg.sessionToken = QString::fromLatin1(kSessionToken);
    vcfg.deviceId = remoteId;

    auto* w = new MainWindow(vcfg);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->resize(1280, 720);
    w->show();
}

void AppMainWindow::onRecentItemActivated(QListWidgetItem* item) {
    if (!item) return;
    remoteIdField_->setText(item->text());
    onConnectClicked();
}

void AppMainWindow::rememberRecent(const QString& remoteId) {
    // Move-to-front MRU. Cap to ten entries so the list doesn't grow
    // unbounded for someone who hops between many machines.
    constexpr int kMaxRecents = 10;
    local_.recentRemoteIds.removeAll(remoteId);
    local_.recentRemoteIds.prepend(remoteId);
    while (local_.recentRemoteIds.size() > kMaxRecents) {
        local_.recentRemoteIds.removeLast();
    }
    SaveLocalConfig(local_);
    refreshRecentList();
}

void AppMainWindow::refreshRecentList() {
    if (!recentList_) return;
    recentList_->clear();
    for (const auto& id : local_.recentRemoteIds) {
        recentList_->addItem(id);
    }
}

void AppMainWindow::showSessionBanner() {
    if (!sessionBanner_) {
        // Lazy build. Frameless, always-on-top, no taskbar entry, no
        // focus stealing — the user should keep working while the banner
        // sits in a corner. Tool + FramelessWindowHint together give the
        // "no title bar, no shadow" look; WA_ShowWithoutActivating stops
        // the alert from grabbing keyboard focus every time it reshows.
        auto* banner = new SessionBannerWidget(local_.deviceId, nullptr,
            Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
            Qt::WindowDoesNotAcceptFocus);
        sessionBanner_ = banner;
        banner->setAttribute(Qt::WA_ShowWithoutActivating);
        banner->setAttribute(Qt::WA_TranslucentBackground, false);
        banner->adjustSize();

        // Action button: single button, semantics depend on state. When a
        // transfer is running it means "cancel"; otherwise it means
        // "send whatever's in my local clipboard". refreshBannerAction()
        // keeps the label in sync.
        banner->setActionCallback([this]() {
            if (xferInFlight_) {
                if (fileXfer_) fileXfer_->cancelActiveTransfer();
            } else if (clipboardHasSendable_) {
                if (fileXfer_) fileXfer_->sendClipboardNow();
            }
        });
    }

    auto* banner = static_cast<SessionBannerWidget*>(sessionBanner_);
    // Only auto-anchor if the user hasn't dragged the banner this run.
    // Once they've dragged it, respect that position across future
    // session start/end cycles.
    if (!banner->hasBeenDragged()) {
        QScreen* scr = QApplication::primaryScreen();
        if (scr) {
            const QRect avail = scr->availableGeometry();
            const int margin = 16;
            const QSize sz = banner->sizeHint().expandedTo(QSize(320, 40));
            banner->resize(sz);
            banner->move(avail.right() - sz.width() - margin,
                         avail.top() + margin);
        }
    }
    banner->show();
    banner->raise();
}

void AppMainWindow::hideSessionBanner() {
    if (sessionBanner_) sessionBanner_->hide();
}

void AppMainWindow::refreshBannerAction() {
    auto* b = static_cast<SessionBannerWidget*>(sessionBanner_);
    if (!b) return;
    // Priority: in-flight cancel takes precedence over "send" prompt so
    // the operator can always abort a runaway upload.
    if (xferInFlight_) {
        b->showCancelPrompt();
    } else if (sessionActive_ && clipboardHasSendable_) {
        b->showSendPrompt(clipboardTopLevelCount_);
    } else {
        b->hideActionButton();
    }
}

void AppMainWindow::onSessionStarted() {
    appendLog(QStringLiteral("[session] 有远程用户已连接 — 你的屏幕正在被观看"));
    sessionActive_ = true;
    showSessionBanner();
    // Re-check clipboard right now — the operator may have Ctrl+C'd
    // BEFORE the session came up, in which case the QClipboard signal
    // already fired but we didn't route it to the banner (no banner yet).
    if (fileXfer_) fileXfer_->onLocalClipboardChanged();
    // If KMBox mode is on but the box is currently dead (heartbeat has
    // been failing since before the session), the banner needs the
    // warning line right away — no boxAliveChanged transition will fire
    // to tell it.
    if (kmboxRouter_ && kmboxRouter_->isEnabled() &&
        !kmboxRouter_->isBoxAlive()) {
        if (auto* b = static_cast<SessionBannerWidget*>(sessionBanner_)) {
            b->showKmboxWarning();
        }
    }
    refreshBannerAction();
    if (tray_) {
        if (!trayIconActive_.isNull()) tray_->setIcon(trayIconActive_);
        tray_->setToolTip(QStringLiteral(
            "Remote Desktop — 正在被远程控制 (本机 ID %1)").arg(local_.deviceId));
        if (tray_->isSystemTrayAvailable()) {
            tray_->showMessage(QStringLiteral("远程连接已建立"),
                               QStringLiteral("有人正在远程控制本机 (本机 ID %1)。"
                                              "如非本人操作请立即断网。").arg(local_.deviceId),
                               QSystemTrayIcon::Warning, 5000);
        }
    }
    // Update status label style so even users who happen to have the main
    // window open notice the change at a glance.
    if (statusLabel_) {
        statusLabel_->setStyleSheet(
            QStringLiteral("color: #d23428; font-weight: bold;"));
    }
}

void AppMainWindow::onSessionEnded() {
    appendLog(QStringLiteral("[session] 远程用户已断开"));
    sessionActive_ = false;
    // No peer = nothing to send to. Cancel any in-flight transfer so the
    // worker doesn't keep pushing bytes into a dead pipe.
    if (xferInFlight_ && fileXfer_) fileXfer_->cancelActiveTransfer();
    hideSessionBanner();
    if (tray_) {
        if (!trayIconIdle_.isNull()) tray_->setIcon(trayIconIdle_);
        tray_->setToolTip(QStringLiteral("Remote Desktop — 本机 ID %1")
                              .arg(local_.deviceId));
        if (tray_->isSystemTrayAvailable()) {
            tray_->showMessage(QStringLiteral("远程连接已结束"),
                               QStringLiteral("远程用户已断开。本机 ID %1")
                                   .arg(local_.deviceId),
                               QSystemTrayIcon::Information, 3000);
        }
    }
    if (statusLabel_) {
        statusLabel_->setStyleSheet(QStringLiteral("color: #666;"));
    }
}
