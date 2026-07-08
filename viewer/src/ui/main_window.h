#pragma once

#include "ui/viewer_session.h"

#include <QMainWindow>
#include <QPointer>
#include <QThread>

class QLabel;
class QProgressBar;
class QPushButton;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const ViewerSession::Config& cfg, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    // Drag-and-drop: drop a file/folder from Explorer into the viewer
    // window to push it to the peer without going through Ctrl+C.
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onStatus(const QString& s);
    void onDisconnected(const QString& reason);

private slots:
    void onClipboardReceived(const QString& text);
    void onLocalClipboardChanged();

private:
    QLabel* status_ = nullptr;
    VideoWidget* video_ = nullptr;
    QThread workerThread_;
    QPointer<ViewerSession> session_;

    // Last text we either RECEIVED from the agent and applied locally, or
    // SENT to the agent in response to a local change. Both pathways write
    // this before triggering the local clipboard, so the resulting
    // dataChanged we observe matches what's already in lastNetText_ and we
    // skip a re-send — that's the loop-break.
    QString lastNetText_;

    class ClipboardFileXfer* fileXfer_ = nullptr;

    // Send / cancel button lives in the viewer's status bar — the viewer
    // has no floating banner (the video pane fills the window), so the
    // status bar is the natural spot for a call-to-action that only
    // appears while the local clipboard has sendable files or while a
    // transfer is running.
    QPushButton*   xferButton_        = nullptr;
    QLabel*        xferLabel_         = nullptr;
    QProgressBar*  xferBar_           = nullptr;
    bool           clipboardHasSendable_   = false;
    int            clipboardTopLevelCount_ = 0;
    bool           xferInFlight_           = false;

    void refreshXferButton();

    // Full-window semi-transparent overlay that appears while a file
    // drag is over the viewer. Provides the "yes, drop here" affordance;
    // hidden the rest of the time. Parented to `this` so it resizes
    // implicitly on resizeEvent (we still ->resize() it explicitly
    // there because the parent geometry change doesn't propagate to
    // top-level-child widgets without manual reflow).
    QLabel* dropOverlay_ = nullptr;
};
