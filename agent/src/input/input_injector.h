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
#include <functional>
#include <string>

namespace input {

// Returns false on bad payload format or SendInput failure; *errorOut tells you why.
bool HandleInputFrame(std::uint8_t msgType,
                      const std::uint8_t* data, std::size_t size,
                      std::string* errorOut);

// Optional override. If set, HandleInputFrame will call this FIRST for every
// frame it receives. Return value semantics:
//   true  → the router handled the frame; SendInput is skipped.
//   false → router declined this specific frame; fall back to SendInput.
//
// This lets the app-level KMBox hardware backend intercept exactly the events
// it can translate (mouse move/button, mapped keys) and drop back to
// SendInput for things it can't. Set nullptr to remove the override.
//
// Router is called from the network read thread — implementations must be
// thread-safe. Set from any thread; the internal store uses a mutex.
using InputRouter = std::function<bool(std::uint8_t msgType,
                                        const std::uint8_t* data,
                                        std::size_t size)>;
void SetInputRouter(InputRouter router);

} // namespace input
