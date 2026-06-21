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
# Shared net library (TcpClient + frame protocol). Lives next door so both
# agent and viewer can pull it in without duplicating files. Add it once at
# the top level (skip if already added by parent CMakeLists, e.g. app/).
# ---------------------------------------------------------------------------
if(NOT TARGET shared_net)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared" shared_build)
endif()

# ---------------------------------------------------------------------------
# agent_core: capture + encode + agent-specific relay client + utilities
# ---------------------------------------------------------------------------
add_library(agent_core STATIC
    src/capture/dxgi_capturer.cpp
    src/encode/h264_encoder.cpp
    src/input/input_injector.cpp
    src/net/agent_relay_client.cpp
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
    shared_net
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

# Bundle the MSVC runtime DLLs (VCRUNTIME140.dll, MSVCP140.dll, etc.) so
# agent.exe runs on a fresh Windows box that doesn't have vc_redist
# installed. Skips the implicit `install()` rule — we only want the var
# populated so we can copy.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
foreach(_msvc_lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    add_custom_command(TARGET agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_msvc_lib}" "$<TARGET_FILE_DIR:agent>"
    )
endforeach()
