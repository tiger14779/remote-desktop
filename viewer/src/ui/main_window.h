#pragma once

#include "ui/viewer_session.h"

#include <QMainWindow>
#include <QPointer>
#include <QThread>

class QLabel;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const ViewerSession::Config& cfg, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

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
};
