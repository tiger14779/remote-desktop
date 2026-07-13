cmake_minimum_required(VERSION 3.20)
project(remote_desktop_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "App targets Windows only.")
endif()

# Find Qt6 at app scope (find_package inside viewer's sub-CMakeLists doesn't
# propagate to us). Must come before add_subdirectory(viewer) so viewer's
# qt_add_executable also uses our found Qt.
if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QT_DIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network)
qt_standard_project_setup()

# Bring agent_core + viewer_core + shared_net into this build by including
# their sub-CMakeLists. Add shared first so the others see TARGET shared_net
# is already defined and skip the add_subdirectory in their own CMakeLists.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared"  shared_build)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../agent"   agent_build  EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../viewer"  viewer_build EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# app: the unified RustDesk-style Qt exe
# ---------------------------------------------------------------------------
qt_add_executable(app
    cmd/app/main.cpp
    src/ui/app_window.cpp
    src/runtime/local_config.cpp
    src/runtime/log_sink.cpp
    src/runtime/agent_runner.cpp
    src/runtime/autostart.cpp
    src/runtime/single_instance.cpp
    src/runtime/kmbox_net.cpp
    src/runtime/kmbox_input.cpp
    src/runtime/permission.cpp
    # Windows .rc — embeds ICON resource so explorer / taskbar shows the
    # icon on remote-desktop.exe itself (before any Qt widget code runs).
    resources/app.rc
)
# Qt resource — using the explicit qt_add_resources form so AUTORCC skipping
# (which happened silently when we just listed the .qrc in the sources list)
# can't leave us with a Q_INIT_RESOURCE-less binary. Icons show as blank in
# QSystemTrayIcon if this is missing.
qt_add_resources(app "app_res"
    PREFIX "/"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/app_icon.png
        resources/app_icon.ico
)
target_include_directories(app PRIVATE
    src
    # For kmbox_input.cpp -> "input/input_injector.h" (lives in agent_core).
    ${CMAKE_CURRENT_SOURCE_DIR}/../agent/src
)
target_link_libraries(app PRIVATE
    agent_core
    viewer_core
    Qt6::Widgets
    Qt6::OpenGLWidgets
    # QtNetwork provides QLocalServer / QLocalSocket for the
    # SingleInstance IPC (see src/runtime/single_instance.h).
    Qt6::Network
    # ws2_32 for KMBox NET's raw WinSock2 UDP socket (kmbox_net.cpp).
    ws2_32
    # Shcore for GetDpiForMonitor (kmbox_input.cpp per-monitor DPI query).
    Shcore
)
# Pull in viewer's UI sources directly (they're not in viewer_core because
# viewer_core is the no-Qt subset; the Qt classes live in viewer/src/ui/).
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/video_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/main_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/viewer_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/clipboard_file_xfer.cpp
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src)

set_target_properties(app PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "remote-desktop"
)

if(MSVC)
    target_compile_options(app PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# Runtime DLL staging is INTENTIONALLY done outside CMake — by the
# VS Code task "App: bundle runtime" (or by release.bat at packaging time).
#
# Why not POST_BUILD? POST_BUILD only fires when the target relinks. On an
# incremental build where remote-desktop.exe is already up to date, MSBuild
# skips link AND skips POST_BUILD — leaving stale or missing DLLs next to
# a current .exe. The dev cycle then looks "succeeded" but launching the
# exe fails with "找不到 avutil-59.dll".
#
# app/scripts/bundle_runtime.ps1 always runs and is what the VS Code task
# (and any future release.bat refresh) calls.
