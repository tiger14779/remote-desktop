#include "ui/main_window.h"

#include "ui/clipboard_file_xfer.h"
#include "ui/video_widget.h"
#include "ui/viewer_session.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(const ViewerSession::Config& cfg, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Remote Desktop Viewer — %1@%2:%3 → %4")
                       .arg(cfg.account)
                       .arg(cfg.host)
                       .arg(cfg.port)
                       .arg(cfg.deviceId));

    video_ = new VideoWidget(this);
    setCentralWidget(video_);

    status_ = new QLabel(QStringLiteral("connecting..."));
    statusBar()->addWidget(status_, 1);

    session_ = new ViewerSession();
    session_->moveToThread(&workerThread_);

    // Cleanup ordering: when the worker thread is about to finish, delete the
    // session on that thread, then quit.
    connect(&workerThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_.data(), &ViewerSession::status,
            this, &MainWindow::onStatus, Qt::QueuedConnection);
    connect(session_.data(), &ViewerSession::frameReady,
            video_, &VideoWidget::onFrame, Qt::QueuedConnection);
    connect(session_.data(), &ViewerSession::disconnected,
            this, &MainWindow::onDisconnected, Qt::QueuedConnection);

    // GUI backpressure feedback: VideoWidget tells ViewerSession every time
    // a paintEvent finishes. ViewerSession uses this to gate emit so the
    // signal queue can never accumulate stale frames. DirectConnection: the
    // slot is a single atomic store, runs synchronously on the GUI thread.
    connect(video_, &VideoWidget::framePainted,
            session_.data(), &ViewerSession::onFramePainted, Qt::DirectConnection);
    connect(video_, &VideoWidget::paintTook, session_.data(),
            [this](int ms) {
                if (!session_) return;
                if (ms > session_->paintMaxMs_.load()) session_->paintMaxMs_.store(ms);
                session_->paintSumMs_.fetch_add(ms);
                session_->paintCount_.fetch_add(1);
            }, Qt::DirectConnection);

    // Input: VideoWidget emits on GUI thread → forward to ViewerSession's
    // thread-safe send methods directly (no Qt::QueuedConnection — the
    // worker thread is blocked inside Run() and would never drain a queue).
    connect(video_, &VideoWidget::inputMouseMove, this,
            [this](std::uint16_t x, std::uint16_t y) {
                if (session_) session_->sendMouseMove(x, y);
            });
    connect(video_, &VideoWidget::inputMouseButton, this,
            [this](std::uint8_t b, bool down) {
                if (session_) session_->sendMouseButton(b, down);
            });
    connect(video_, &VideoWidget::inputKey, this,
            [this](std::uint16_t vk, bool down) {
                if (session_) session_->sendKey(vk, down);
            });

    // Clipboard sync (bidirectional, text). Outbound: any local change is
    // forwarded to the agent unless we ourselves just set it. Inbound: text
    // arrives via a queued signal from the network thread, we apply it
    // locally and prime lastNetText_ so the resulting dataChanged echo is
    // filtered out instead of bouncing back to the agent.
    connect(session_.data(), &ViewerSession::clipboardReceived,
            this, &MainWindow::onClipboardReceived, Qt::QueuedConnection);
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
            this, &MainWindow::onLocalClipboardChanged);

    // Cursor shape mirror is disabled — see agent_runner.cpp note. The
    // signal stays exposed by ViewerSession but no slot listens, so the
    // VideoWidget keeps Qt's default arrow cursor.

    // Clipboard file transfer — same QClipboard subscription as the text
    // path above; ClipboardFileXfer inspects mimeData and routes either to
    // text-sync (handled by onLocalClipboardChanged) or file-xfer (here).
    fileXfer_ = new ClipboardFileXfer(this);
    fileXfer_->setSendFn([this](std::uint8_t t, const std::uint8_t* d, std::size_t s) {
        if (session_) {
            session_->sendFrameRaw(
                t, QByteArray(reinterpret_cast<const char*>(d),
                              static_cast<int>(s)));
        }
    });
    connect(session_.data(), &ViewerSession::fileXferFrameReceived,
            this, [this](std::uint8_t t, const QByteArray& p) {
                if (fileXfer_) {
                    fileXfer_->onPeerFrame(
                        t, reinterpret_cast<const std::uint8_t*>(p.constData()),
                        static_cast<std::size_t>(p.size()));
                }
            }, Qt::QueuedConnection);
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
            this, [this]() { if (fileXfer_) fileXfer_->onLocalClipboardChanged(); });
    connect(fileXfer_, &ClipboardFileXfer::log, this,
            [](const QString& s) { qInfo("%s", qUtf8Printable(s)); });

    workerThread_.start();
    QMetaObject::invokeMethod(session_.data(),
                              "start", Qt::QueuedConnection,
                              Q_ARG(ViewerSession::Config, cfg));
}

MainWindow::~MainWindow() {
    if (session_) session_->requestStop();
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (session_) session_->requestStop();
    e->accept();
}

void MainWindow::onStatus(const QString& s) {
    status_->setText(s);
}

void MainWindow::onDisconnected(const QString& reason) {
    status_->setText(QStringLiteral("disconnected: %1").arg(reason));
}

void MainWindow::onClipboardReceived(const QString& text) {
    if (text == lastNetText_) return;  // already in sync
    lastNetText_ = text;
    QApplication::clipboard()->setText(text);
}

void MainWindow::onLocalClipboardChanged() {
    if (!session_) return;
    const QString text = QApplication::clipboard()->text();
    if (text == lastNetText_) return;  // echo from our own setText
    if (text.isEmpty()) return;        // don't sync empty (e.g. image copy)
    lastNetText_ = text;
    session_->sendClipboardText(text);
}
