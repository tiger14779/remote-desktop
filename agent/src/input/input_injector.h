#pragma once

// SendInput-based injection of viewer-side input events.
//
// Payload byte layouts (also documented in viewer/src/ui/video_widget.cpp):
//
//   MOUSE_MOVE   (4 bytes):  [u16 BE x][u16 BE y]      0..65535 over agent's
//                                                       primary monitor
//   MOUSE_BUTTON (2 bytes):  [button][down]            button: 0=L,1=R,2=M,3=X1,4=X2
//   KEY_EVENT    (4 bytes):  [u16 BE vk][down][rsv=0]  vk = Windows VK_*
//
// HandleInputFrame is the single entry point: it dispatches on msgType and
// validates the payload size before calling Windows.

#include <cstddef>
#include <cstdint>
#include <string>

namespace input {

// Returns false on bad payload format or SendInput failure; *errorOut tells you why.
bool HandleInputFrame(std::uint8_t msgType,
                      const std::uint8_t* data, std::size_t size,
                      std::string* errorOut);

} // namespace input
