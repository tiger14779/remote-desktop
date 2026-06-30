#pragma once

#include "runtime/agent_runner.h"
#include "runtime/local_config.h"

#include <QMainWindow>
#include <QPointer>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;

// RustDesk-style single window. Always runs the AgentRunner in the
// background ("this machine can be controlled"); clicking Connect opens
// a viewer window pointed at the entered remote device.
class AppMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AppMainWindow(const LocalConfig& localCfg, QWidget* parent = nullptr);
    ~AppMainWindow() override;

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

private:
    void appendLog(const QString& s);
    void rememberRecent(const QString& remoteId);
    void refreshRecentList();

    LocalConfig local_;
    AgentRunner agent_;

    // See MainWindow::lastNetText_ — same loop-break trick for the agent
    // side of clipboard sync.
    QString lastNetText_;

    QLineEdit*   localIdField_ = nullptr;
    QLabel*      statusLabel_ = nullptr;

    QLineEdit*   remoteIdField_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QListWidget* recentList_ = nullptr;

    QPlainTextEdit* logView_ = nullptr;

    class ClipboardFileXfer* fileXfer_ = nullptr;
};
