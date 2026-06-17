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

private:
    QLabel* status_ = nullptr;
    VideoWidget* video_ = nullptr;
    QThread workerThread_;
    QPointer<ViewerSession> session_;
};
