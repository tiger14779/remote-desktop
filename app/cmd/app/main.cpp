#include "runtime/local_config.h"
#include "ui/app_window.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Remote Desktop"));
    app.setOrganizationName(QStringLiteral("newboos"));

    LocalConfig cfg = LoadOrCreateLocalConfig();

    AppMainWindow win(cfg);
    win.show();
    return app.exec();
}
