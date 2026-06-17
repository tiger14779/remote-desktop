// Viewer Qt entry point. Parses CLI args for relay/account/device, builds the
// MainWindow which spins up the background ViewerSession, and runs the event
// loop.

#include "ui/main_window.h"
#include "ui/viewer_session.h"

#include <QApplication>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <iostream>

namespace {

bool nextArg(const QStringList& args, int& i, QString* out) {
    if (i + 1 >= args.size()) return false;
    *out = args.at(++i);
    return true;
}

bool splitHostPort(const QString& hp, QString* host, int* port) {
    int pos = hp.lastIndexOf(':');
    if (pos < 0) return false;
    *host = hp.left(pos);
    *port = hp.mid(pos + 1).toInt();
    return *port > 0;
}

void printUsage() {
    std::cerr <<
        "usage: viewer [-relay host:port] [-account NAME] [-session-token TOK] "
        "[-device-id ID]\n";
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qRegisterMetaType<ViewerSession::Config>("ViewerSession::Config");

    ViewerSession::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 7100;
    cfg.account = "alice";
    cfg.sessionToken = "viewer-token-alice";
    cfg.deviceId = "device-001";

    QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args.at(i);
        if (a == "-relay") {
            QString hp;
            if (!nextArg(args, i, &hp) || !splitHostPort(hp, &cfg.host, &cfg.port)) {
                printUsage(); return 1;
            }
        } else if (a == "-account") {
            if (!nextArg(args, i, &cfg.account)) { printUsage(); return 1; }
        } else if (a == "-session-token") {
            if (!nextArg(args, i, &cfg.sessionToken)) { printUsage(); return 1; }
        } else if (a == "-device-id") {
            if (!nextArg(args, i, &cfg.deviceId)) { printUsage(); return 1; }
        } else if (a == "-h" || a == "--help") {
            printUsage();
            return 0;
        } else {
            std::cerr << "unknown arg: " << a.toStdString() << "\n";
            printUsage();
            return 1;
        }
    }

    MainWindow win(cfg);
    win.resize(1280, 720);
    win.show();
    return app.exec();
}
