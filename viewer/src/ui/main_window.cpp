#include "ui/main_window.h"

#include "ui/video_widget.h"
#include "ui/viewer_session.h"

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
