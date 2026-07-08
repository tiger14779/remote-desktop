#include "ui/main_window.h"

#include "ui/clipboard_file_xfer.h"
#include "ui/video_widget.h"
#include "ui/viewer_session.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
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

    // File-xfer widgets in the status bar (right side). Hidden until the
    // local clipboard picks up file URLs or a transfer starts. Layout:
    //   [ status_ (stretch) | xferLabel | xferBar | xferButton ]
    xferLabel_ = new QLabel;
    xferLabel_->setStyleSheet(QStringLiteral("color: #555;"));
    xferLabel_->hide();
    xferBar_ = new QProgressBar;
    xferBar_->setRange(0, 100);
    xferBar_->setMaximumWidth(140);
    xferBar_->setTextVisible(true);
    xferBar_->hide();
    xferButton_ = new QPushButton;
    xferButton_->setCursor(Qt::PointingHandCursor);
    xferButton_->hide();
    statusBar()->addPermanentWidget(xferLabel_);
    statusBar()->addPermanentWidget(xferBar_);
    statusBar()->addPermanentWidget(xferButton_);

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
    connect(video_, &VideoWidget::inputMouseWheel, this,
            [this](std::int16_t d) {
                if (session_) session_->sendMouseWheel(d);
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

    // Explicit-send wiring (mirror of the agent-side banner logic in
    // AppMainWindow). Clipboard changes are detection-only; the button
    // is the operator's opt-in to actually push bytes over the WAN.
    connect(fileXfer_, &ClipboardFileXfer::localFilesAvailable, this,
            [this](bool avail, int n) {
                clipboardHasSendable_ = avail;
                clipboardTopLevelCount_ = n;
                refreshXferButton();
            });
    connect(fileXfer_, &ClipboardFileXfer::xferStarted, this,
            [this](bool sending, qint64 total, int fileCount) {
                xferInFlight_ = true;
                const QString dir = sending ? QStringLiteral("发送")
                                            : QStringLiteral("接收");
                xferLabel_->setText(QStringLiteral("%1 %2 个文件, %3 KB")
                                        .arg(dir).arg(fileCount)
                                        .arg(total / 1024));
                xferLabel_->show();
                xferBar_->setValue(0);
                xferBar_->show();
                refreshXferButton();
            }, Qt::QueuedConnection);
    connect(fileXfer_, &ClipboardFileXfer::xferProgress, this,
            [this](bool sending, qint64 done, qint64 total, int pct) {
                const QString dir = sending ? QStringLiteral("发送")
                                            : QStringLiteral("接收");
                xferLabel_->setText(QStringLiteral("%1 中: %2 / %3 KB")
                                        .arg(dir).arg(done / 1024)
                                        .arg(total / 1024));
                xferBar_->setValue(pct);
            }, Qt::QueuedConnection);
    connect(fileXfer_, &ClipboardFileXfer::xferFinished, this,
            [this](bool, bool, qint64) {
                xferInFlight_ = false;
                xferLabel_->hide();
                xferBar_->hide();
                refreshXferButton();
            }, Qt::QueuedConnection);

    // Single button, semantics depend on state — same dispatch trick as
    // AppMainWindow's banner action.
    connect(xferButton_, &QPushButton::clicked, this, [this]() {
        if (xferInFlight_) {
            if (fileXfer_) fileXfer_->cancelActiveTransfer();
        } else if (clipboardHasSendable_) {
            if (fileXfer_) fileXfer_->sendClipboardNow();
        }
    });

    // Drag-and-drop: accept dropped files/folders anywhere in the window.
    // ClipboardFileXfer::sendFilesNow will do the same validation as the
    // Ctrl+C path (loop-break, concurrency guard, enumeration).
    setAcceptDrops(true);

    // Big translucent overlay label. Hidden except during an active
    // dragEnter. Positioned to cover the whole client area in resizeEvent.
    dropOverlay_ = new QLabel(this);
    dropOverlay_->setAlignment(Qt::AlignCenter);
    dropOverlay_->setText(QStringLiteral("松开鼠标 → 发送到被控端"));
    dropOverlay_->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background-color: rgba(30, 100, 30, 180);"
        "  color: white;"
        "  font-size: 22px;"
        "  font-weight: bold;"
        "  border: 3px dashed white;"
        "  border-radius: 12px;"
        "}"));
    // Overlay must not eat mouse events targeted at what's underneath
    // when we're NOT dragging. Since we hide() it outside of dragEnter,
    // this is defensive.
    dropOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    dropOverlay_->hide();

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

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    // Accept only URL payloads that reference at least one local file.
    // Explorer, most file managers, and even browser-sourced files (via
    // "download link") land as file:// URLs here.
    if (!e->mimeData() || !e->mimeData()->hasUrls()) return;
    bool anyLocal = false;
    for (const auto& u : e->mimeData()->urls()) {
        if (u.isLocalFile()) { anyLocal = true; break; }
    }
    if (!anyLocal) return;
    if (dropOverlay_) {
        dropOverlay_->resize(size());
        dropOverlay_->raise();
        dropOverlay_->show();
    }
    e->acceptProposedAction();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent* e) {
    if (dropOverlay_) dropOverlay_->hide();
    e->accept();
}

void MainWindow::dropEvent(QDropEvent* e) {
    if (dropOverlay_) dropOverlay_->hide();
    if (!e->mimeData() || !e->mimeData()->hasUrls()) return;
    if (!fileXfer_) return;
    // Filter down to local files — file managers occasionally throw in
    // a browser tab URL alongside the file, and enumerate() would just
    // skip those anyway.
    QList<QUrl> urls;
    for (const auto& u : e->mimeData()->urls()) {
        if (u.isLocalFile()) urls.append(u);
    }
    if (urls.isEmpty()) return;
    fileXfer_->sendFilesNow(urls);
    e->acceptProposedAction();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    // Keep the drag overlay covering the whole client area. Only matters
    // when it's currently visible (mid-drag) but cheap to always resize.
    if (dropOverlay_) dropOverlay_->resize(e->size());
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

void MainWindow::refreshXferButton() {
    // Same priority as the agent-side banner: cancel takes precedence so
    // a runaway upload can always be aborted; send prompt only when
    // clipboard has files and we're not currently sending.
    if (xferInFlight_) {
        xferButton_->setText(QStringLiteral("取消传输"));
        xferButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #d23428; color: white;"
            " border-radius: 3px; padding: 2px 8px; font-weight: bold; }"
            "QPushButton:hover { background-color: #b02820; }"));
        xferButton_->show();
    } else if (clipboardHasSendable_) {
        xferButton_->setText(QStringLiteral("→ 发送 %1 项到被控端")
                                  .arg(clipboardTopLevelCount_));
        xferButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #2a7a2a; color: white;"
            " border-radius: 3px; padding: 2px 8px; font-weight: bold; }"
            "QPushButton:hover { background-color: #226622; }"));
        xferButton_->show();
    } else {
        xferButton_->hide();
    }
}
