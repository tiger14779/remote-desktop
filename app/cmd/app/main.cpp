#include "runtime/local_config.h"
#include "runtime/log_sink.h"
#include "ui/app_window.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Remote Desktop"));
    app.setOrganizationName(QStringLiteral("newboos"));

    // Capture every std::cerr / std::cout line into the log panel. Has to
    // happen before any code that writes diagnostics — but the AgentRunner
    // worker thread doesn't start until AppMainWindow constructs, so
    // doing it here is plenty early.
    LogSink::instance()->install();

    LocalConfig cfg = LoadOrCreateLocalConfig();

    AppMainWindow win(cfg);
    win.show();
    return app.exec();
}
