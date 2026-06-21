#include "ui/app_window.h"

#include "runtime/log_sink.h"

// Viewer's MainWindow class lives at viewer/src/ui/main_window.h; that path
// is on our include search list via app/CMakeLists.
#include "ui/main_window.h"
#include "ui/viewer_session.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>

// Hardcoded relay endpoint — change here for now, will be configurable later.
static constexpr const char* kRelayHost = "192.168.1.29";
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
        mono.setPointSize(mono.pointSize() + 2);
        localIdField_->setFont(mono);
        idRow->addWidget(localIdField_, 1);
        auto* copyId = new QPushButton(QStringLiteral("复制"));
        idRow->addWidget(copyId);
        connect(copyId, &QPushButton::clicked, this, [this]() {
            copyToClipboard(localIdField_);
        });
        gl->addLayout(idRow);

        auto* codeRow = new QHBoxLayout;
        codeRow->addWidget(new QLabel(QStringLiteral("验证码:")));
        localCodeField_ = new QLineEdit(local_.verifyCode);
        localCodeField_->setReadOnly(true);
        localCodeField_->setFont(mono);
        codeRow->addWidget(localCodeField_, 1);
        auto* copyCode = new QPushButton(QStringLiteral("复制"));
        codeRow->addWidget(copyCode);
        connect(copyCode, &QPushButton::clicked, this, [this]() {
            copyToClipboard(localCodeField_);
        });
        gl->addLayout(codeRow);

        statusLabel_ = new QLabel(QStringLiteral("启动中..."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #666;"));
        gl->addWidget(statusLabel_);

        root->addWidget(g);
    }

    // === Remote section: "connect to" ======================================
    {
        auto* g = new QGroupBox(QStringLiteral("连接对方"));
        auto* gl = new QVBoxLayout(g);

        auto* idRow = new QHBoxLayout;
        idRow->addWidget(new QLabel(QStringLiteral("远程 ID:")));
        remoteIdField_ = new QLineEdit;
        remoteIdField_->setPlaceholderText(QStringLiteral("对方的 9 位 ID"));
        idRow->addWidget(remoteIdField_, 1);
        gl->addLayout(idRow);

        auto* codeRow = new QHBoxLayout;
        codeRow->addWidget(new QLabel(QStringLiteral("验证码:")));
        remoteCodeField_ = new QLineEdit;
        remoteCodeField_->setPlaceholderText(QStringLiteral("对方的 8 位验证码"));
        codeRow->addWidget(remoteCodeField_, 1);
        gl->addLayout(codeRow);

        connectButton_ = new QPushButton(QStringLiteral("连接"));
        connectButton_->setMinimumHeight(36);
        QFont bigger = connectButton_->font();
        bigger.setPointSize(bigger.pointSize() + 2);
        bigger.setBold(true);
        connectButton_->setFont(bigger);
        connect(connectButton_, &QPushButton::clicked, this, &AppMainWindow::onConnectClicked);
        gl->addWidget(connectButton_);

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

    // Spin up the always-on agent connection.
    AgentRunner::Config rcfg;
    rcfg.relayHost = QString::fromLatin1(kRelayHost);
    rcfg.relayPort = kRelayPort;
    rcfg.deviceId = local_.deviceId;
    // For now reuse the master device token. Future: per-device dynamic
    // registration where the relay accepts an arbitrary deviceId+token.
    rcfg.deviceToken = QString::fromLatin1(kDeviceToken);
    // 60 fps so dragging actually feels smooth (30 fps drag is fundamentally
    // stuttery, regardless of latency). Bitrate scaled up to give the
    // encoder enough budget per frame at the new cadence.
    rcfg.fps = 60;
    rcfg.bitrateKbps = 12000;
    rcfg.maxHeight = 1440;
    connect(&agent_, &AgentRunner::status, this,
            &AppMainWindow::onAgentStatus, Qt::QueuedConnection);

    appendLog(QStringLiteral("app started — local id=%1 code=%2")
                  .arg(local_.deviceId, local_.verifyCode));
    agent_.start(rcfg);
}

AppMainWindow::~AppMainWindow() = default;

void AppMainWindow::closeEvent(QCloseEvent* e) {
    agent_.stop();
    e->accept();
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

void AppMainWindow::onConnectClicked() {
    const QString remoteId = remoteIdField_->text().trimmed();
    if (remoteId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请输入远程 ID"));
        return;
    }
    appendLog(QStringLiteral("connecting to remote id=%1").arg(remoteId));
    // verifyCode is recorded for the operator's mental model but the MVP
    // relay still verifies via the master session_token. T2-7 onwards
    // wires per-target verification.

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
