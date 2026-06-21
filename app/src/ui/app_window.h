#pragma once

#include "runtime/agent_runner.h"
#include "runtime/local_config.h"

#include <QMainWindow>
#include <QPointer>

class QLabel;
class QLineEdit;
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
    void onAgentStatus(const QString& s);
    void onLogLine(const QString& s);

private:
    void appendLog(const QString& s);

    LocalConfig local_;
    AgentRunner agent_;

    QLineEdit* localIdField_ = nullptr;
    QLineEdit* localCodeField_ = nullptr;
    QLabel*    statusLabel_ = nullptr;

    QLineEdit* remoteIdField_ = nullptr;
    QLineEdit* remoteCodeField_ = nullptr;
    QPushButton* connectButton_ = nullptr;

    QPlainTextEdit* logView_ = nullptr;
};
