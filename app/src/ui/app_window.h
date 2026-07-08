#pragma once

#include "runtime/agent_runner.h"
#include "runtime/local_config.h"

#include <QIcon>
#include <QMainWindow>
#include <QPointer>

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSystemTrayIcon;
class QMenu;
class QWidget;

// RustDesk-style single window. Always runs the AgentRunner in the
// background ("this machine can be controlled"); clicking Connect opens
// a viewer window pointed at the entered remote device.
class AppMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AppMainWindow(const LocalConfig& localCfg, QWidget* parent = nullptr);
    ~AppMainWindow() override;

    // Un-hide + un-minimise + steal foreground focus. Handles the Windows
    // foreground-lock corner case (see the impl for the AttachThreadInput
    // dance) so this reliably surfaces the window even when we're called
    // from a background context — tray-icon click, single-instance IPC
    // wake, etc.
    void activateFromBackground();

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void copyToClipboard(QLineEdit* src);
    void onConnectClicked();
    void onRecentItemActivated(QListWidgetItem* item);
    void onAgentStatus(const QString& s);
    void onLogLine(const QString& s);
    void onAgentClipboardReceived(const QString& text);
    void onLocalClipboardChanged();
    void onSessionStarted();
    void onSessionEnded();

private:
    void appendLog(const QString& s);
    void rememberRecent(const QString& remoteId);
    void refreshRecentList();
    void setupTrayIcon();
    // Small always-on-top banner that appears in the corner of the primary
    // screen whenever a viewer is attached. Visible even after the main
    // window is hidden to tray — its whole point is to make sure the local
    // user always knows their screen is being watched.
    void showSessionBanner();
    void hideSessionBanner();
    // Recompute what the banner's action button should show. Called from
    // any of the state-change slots.
    void refreshBannerAction();

    LocalConfig local_;
    AgentRunner agent_;

    // See MainWindow::lastNetText_ — same loop-break trick for the agent
    // side of clipboard sync.
    QString lastNetText_;

    QLineEdit*   localIdField_ = nullptr;
    QLabel*      statusLabel_ = nullptr;
    QCheckBox*   autoStartBox_ = nullptr;  // register HKCU\Run at logon

    // KMBox hardware input backend (bypasses WeChat Work detection).
    QLineEdit*   kmboxIpField_    = nullptr;
    QLineEdit*   kmboxPortField_  = nullptr;
    QLineEdit*   kmboxUuidField_  = nullptr;
    QCheckBox*   kmboxEnableBox_  = nullptr;
    QLabel*      kmboxStatusLabel_ = nullptr;
    class KmboxInputRouter* kmboxRouter_ = nullptr;

    QLineEdit*   remoteIdField_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QListWidget* recentList_ = nullptr;

    QPlainTextEdit* logView_ = nullptr;

    class ClipboardFileXfer* fileXfer_ = nullptr;

    // System tray: minimise-to-tray so the agent keeps running when the
    // window is closed. Actual quit is only via the tray menu's Exit item.
    QSystemTrayIcon* tray_ = nullptr;
    QMenu*           trayMenu_ = nullptr;
    bool             reallyQuit_ = false;

    // Frameless always-on-top banner shown while a viewer is attached. Lives
    // for the app's lifetime but is normally hidden — created lazily on the
    // first session start.
    QWidget*         sessionBanner_ = nullptr;
    // Which UI state the banner's action button should reflect. Combined
    // from session-active, files-in-local-clipboard, and xfer-in-progress
    // via refreshBannerAction(). Cached so re-emitting the same signal
    // (e.g. QClipboard rapid-fires dataChanged) doesn't reshuffle labels.
    bool             sessionActive_          = false;
    bool             clipboardHasSendable_   = false;
    int              clipboardTopLevelCount_ = 0;
    bool             xferInFlight_           = false;
    // Persistent tray notification anchor: the same tray icon flashes / has
    // its tooltip swapped while the session is active. We restore both on
    // session end.
    QIcon            trayIconIdle_;
    QIcon            trayIconActive_;
};
