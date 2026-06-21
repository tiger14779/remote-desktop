cmake_minimum_required(VERSION 3.20)
project(remote_desktop_viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Viewer currently targets Windows only (matches Agent + Relay setup).")
endif()

# ---------------------------------------------------------------------------
# FFmpeg discovery (same pattern as agent/).
# ---------------------------------------------------------------------------
if(NOT FFMPEG_ROOT)
    if(DEFINED ENV{VCPKG_ROOT})
        set(FFMPEG_ROOT "$ENV{VCPKG_ROOT}/installed/x64-windows")
    endif()
endif()
if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg headers not found. Pass -DFFMPEG_ROOT=<path> or set VCPKG_ROOT.")
endif()
message(STATUS "FFmpeg root: ${FFMPEG_ROOT}")

# ---------------------------------------------------------------------------
# Qt6. Default to $QT_DIR (e.g. E:/.../qt/6.7.3/msvc2022_64) but the caller
# can override via -DCMAKE_PREFIX_PATH.
# ---------------------------------------------------------------------------
if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QT_DIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
message(STATUS "Qt6 ${Qt6_VERSION} at ${Qt6_DIR}")

# ---------------------------------------------------------------------------
# viewer_core: pure C++, no Qt — net + decode + utilities. Keeps tests cheap.
# ---------------------------------------------------------------------------
add_library(viewer_core STATIC
    src/decode/h264_decoder.cpp
    src/net/tcp_client.cpp
    src/net/frame.cpp
    src/net/relay_client.cpp
)
target_include_directories(viewer_core PUBLIC src)
target_include_directories(viewer_core SYSTEM PUBLIC "${FFMPEG_ROOT}/include")
target_link_directories(viewer_core PUBLIC "${FFMPEG_ROOT}/lib")
target_link_libraries(viewer_core PUBLIC avcodec avutil swscale)
if(MSVC)
    target_compile_options(viewer_core PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# ---------------------------------------------------------------------------
# decode_test: feed a .h264 file through the decoder and dump first frame as
# BMP. Useful for verifying the decode path on its own.
# ---------------------------------------------------------------------------
add_executable(decode_test cmd/decode_test/main.cpp)
target_link_libraries(decode_test PRIVATE viewer_core)

# ---------------------------------------------------------------------------
# viewer: Qt GUI application
# ---------------------------------------------------------------------------
qt_add_executable(viewer
    cmd/viewer/main.cpp
    src/ui/video_widget.cpp
    src/ui/main_window.cpp
    src/ui/viewer_session.cpp
)
target_link_libraries(viewer PRIVATE viewer_core Qt6::Widgets)
set_target_properties(viewer PROPERTIES WIN32_EXECUTABLE ON)

# Copy FFmpeg DLLs next to viewer.exe so it runs without PATH edits. Qt's
# own DLLs are placed by `windeployqt` (run separately if you want a redist).
add_custom_command(TARGET viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:viewer>"
)

# Tell developers how to fetch Qt DLLs into the build dir.
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qtBin "${_qmake}" DIRECTORY)
add_custom_command(TARGET viewer POST_BUILD
    COMMAND "${_qtBin}/windeployqt.exe" --no-translations --no-system-d3d-compiler
            --no-opengl-sw "$<TARGET_FILE:viewer>"
    COMMENT "Running windeployqt to bundle Qt runtime"
)

# Bundle MSVC runtime so viewer.exe runs on a fresh box without vc_redist.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
foreach(_msvc_lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    add_custom_command(TARGET viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_msvc_lib}" "$<TARGET_FILE_DIR:viewer>"
    )
endforeach()

# decode_test ends up in the same Release dir as viewer, so it picks up the
# FFmpeg DLLs from viewer's POST_BUILD copy. We deliberately do NOT add the
# same POST_BUILD to decode_test — when /m parallel build is on, two targets
# concurrently invoking `cmake -E copy_directory` on the same destination race
# and one fails.
