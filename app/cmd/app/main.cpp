#include "runtime/local_config.h"
#include "runtime/log_sink.h"
#include "runtime/single_instance.h"
#include "ui/app_window.h"

#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

int main(int argc, char** argv) {
    // Per-monitor DPI awareness. Critical for the KMBox mouse routing path
    // (see kmbox_input.cpp): GetSystemMetrics and GetCursorPos must return
    // physical pixels, not Windows' virtualised coordinate space. Without
    // this, on a 2560x1440 @ 150% display the "primary monitor size" comes
    // back as 1706x960 while the cursor's coord space is different, and
    // computed deltas overshoot. Must be called before ANY window / DC is
    // created — that's why we do it here at the top of main.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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

    // Single-instance guard. If another copy is already running for this
    // user, poke it to raise its main window and exit — the user just
    // clicked "launch again" and expects the running window to pop up,
    // not a second agent connection registered against the relay under
    // the same device ID. The key includes the OS username via the
    // per-user QLocalServer socket, so two logged-in users on the same
    // box (fast user switch / RDP) each get their own primary.
    SingleInstance guard(QStringLiteral("newboos.remote_desktop.singleton.v1"));
    if (!guard.tryStartAsPrimary()) {
        guard.notifyRunningInstance();
        timeEndPeriod(1);
        return 0;
    }
    // Global app icon — drives every top-level window's title-bar icon and,
    // combined with the .rc-embedded ICON resource, the taskbar entry.
    app.setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
    // Close-to-tray only works if we tell Qt NOT to quit when the last
    // visible window disappears. Otherwise closeEvent's hide()+ignore()
    // still causes the whole event loop to exit because "no top-level
    // windows visible" is Qt's default quit trigger. Real quit path is
    // AppMainWindow's tray-menu "退出" (QApplication::quit).
    app.setQuitOnLastWindowClosed(false);

    // Capture every std::cerr / std::cout line into the log panel. Has to
    // happen before any code that writes diagnostics — but the AgentRunner
    // worker thread doesn't start until AppMainWindow constructs, so
    // doing it here is plenty early.
    LogSink::instance()->install();

    LocalConfig cfg = LoadOrCreateLocalConfig();

    AppMainWindow win(cfg);
    // A secondary process connecting to us via the SingleInstance socket
    // means "the user tried to launch again" — bring the main window to
    // the foreground even if it was hidden to tray.
    QObject::connect(&guard, &SingleInstance::wakeupRequested,
                     &win, &AppMainWindow::activateFromBackground);
    win.show();
    int rc = app.exec();
    timeEndPeriod(1);
    return rc;
}
