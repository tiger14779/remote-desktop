#include "ui/app_window.h"

#include "runtime/log_sink.h"

// Viewer's MainWindow class lives at viewer/src/ui/main_window.h; that path
// is on our include search list via app/CMakeLists.
#include "ui/clipboard_file_xfer.h"
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
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>

// Hardcoded relay endpoint — change here for now, will be configurable later.
static constexpr const char* kRelayHost = "192.243.119.207";
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

        root->addWidget(g);
    }

    // === Remote section: "connect to" ======================================
    {
        auto* g = new QGroupBox(QStringLiteral("连接对方"));
        auto* gl = new QVBoxLayout(g);

        auto* idRow = new QHBoxLayout;
        idRow->addWidget(new QLabel(QStringLiteral("远程 ID:")));
        remoteIdField_ = new QLineEdit;
        remoteIdField_->setPlaceholderText(QStringLiteral("对方的 5 位 ID"));
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

    appendLog(QStringLiteral("app started — local id=%1").arg(local_.deviceId));
    refreshRecentList();
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
