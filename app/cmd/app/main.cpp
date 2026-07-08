#include "runtime/local_config.h"
#include "runtime/log_sink.h"
#include "ui/app_window.h"

#include <QApplication>
#include <QSurfaceFormat>

#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

int main(int argc, char** argv) {
    // Bump the multimedia timer to 1ms resolution so std::this_thread::
    // sleep_until in the agent's frame pacer actually hits 60fps instead
    // of getting rounded to the Windows default 15.6ms scheduler tick
    // (which caps us at ~32fps — the exact number we were seeing). Paired
    // timeEndPeriod after event loop exits keeps other apps' battery life
    // measurement honest.
    timeBeginPeriod(1);

    // Disable vsync on the viewer's QOpenGLWidget so paint results appear
    // as soon as the shader draws, instead of blocking until the next
    // display refresh (up to 16ms at 60Hz). Trade-off: fast drags can
    // show mid-refresh tearing lines. For a remote-desktop workflow the
    // latency win is worth it — RustDesk / TeamViewer default the same way.
    QSurfaceFormat sf;
    sf.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(sf);

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
    int rc = app.exec();
    timeEndPeriod(1);
    return rc;
}
