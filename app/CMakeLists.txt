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
find_package(Qt6 REQUIRED COMPONENTS Widgets)
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
)
target_include_directories(app PRIVATE src)
target_link_libraries(app PRIVATE
    agent_core
    viewer_core
    Qt6::Widgets
)
# Pull in viewer's UI sources directly (they're not in viewer_core because
# viewer_core is the no-Qt subset; the Qt classes live in viewer/src/ui/).
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/video_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/main_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/ui/viewer_session.cpp
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
