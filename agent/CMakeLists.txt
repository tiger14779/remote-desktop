cmake_minimum_required(VERSION 3.20)
project(remote_desktop_agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Agent currently targets Windows only (DXGI Desktop Duplication).")
endif()

# ---------------------------------------------------------------------------
# FFmpeg discovery.
#
# Two modes are supported:
#   1. Caller passes -DFFMPEG_ROOT=<path>  (path containing include/ and lib/)
#   2. VCPKG_ROOT is set and ffmpeg is installed at
#      $VCPKG_ROOT/installed/x64-windows  (classic vcpkg install)
# ---------------------------------------------------------------------------
if(NOT FFMPEG_ROOT)
    if(DEFINED ENV{VCPKG_ROOT})
        set(FFMPEG_ROOT "$ENV{VCPKG_ROOT}/installed/x64-windows")
    endif()
endif()
if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg headers not found. Pass -DFFMPEG_ROOT=<path> or set VCPKG_ROOT "
        "env var so $VCPKG_ROOT/installed/x64-windows points at the install.")
endif()
message(STATUS "FFmpeg root: ${FFMPEG_ROOT}")

# ---------------------------------------------------------------------------
# agent_core: capture + encode + utilities (no networking yet)
# ---------------------------------------------------------------------------
add_library(agent_core STATIC
    src/capture/dxgi_capturer.cpp
    src/encode/h264_encoder.cpp
    src/input/input_injector.cpp
    src/net/tcp_client.cpp
    src/net/frame.cpp
    src/net/relay_client.cpp
    src/util/bmp_writer.cpp
)
target_include_directories(agent_core
    PUBLIC
        src
)
# Mark FFmpeg as SYSTEM include so its C4244 warnings don't pollute /W4 builds.
target_include_directories(agent_core SYSTEM PUBLIC "${FFMPEG_ROOT}/include")
target_link_directories(agent_core PUBLIC "${FFMPEG_ROOT}/lib")
target_link_libraries(agent_core PUBLIC
    d3d11 dxgi dxguid
    avcodec avutil swscale
)
if(MSVC)
    # /utf-8 silences C4819 on machines where the system code page isn't UTF-8
    # (e.g. Chinese Windows defaults to GB2312).
    target_compile_options(agent_core PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# ---------------------------------------------------------------------------
# Smoke-test executables. Each phase has its own so they can be run in
# isolation while bringing the agent up.
# ---------------------------------------------------------------------------
add_executable(capture_test cmd/capture_test/main.cpp)
target_link_libraries(capture_test PRIVATE agent_core)

add_executable(encode_test cmd/encode_test/main.cpp)
target_link_libraries(encode_test PRIVATE agent_core)

add_executable(agent cmd/agent/main.cpp)
target_link_libraries(agent PRIVATE agent_core)

# Copy the FFmpeg runtime DLLs next to the binaries so they can run
# without manually editing PATH.
add_custom_command(TARGET agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:agent>"
)
