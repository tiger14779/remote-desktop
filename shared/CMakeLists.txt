cmake_minimum_required(VERSION 3.20)

# Common protocol layer used by both agent and viewer. Lives in a separate
# static lib so a single binary (the unified app/) can link both sides
# without TcpClient / proto:: duplicate-symbol conflicts.

add_library(shared_net STATIC
    net/tcp_client.cpp
    net/frame.cpp
    udp/udp_socket.cpp
    udp/video_xport.cpp
)
target_include_directories(shared_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(shared_net PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8)
endif()
