#pragma once

#include <cstdint>
#include <string>

// Phase 2a helper: write a BGRA8 top-down buffer to a 32-bit BMP.
// rowPitch is the byte stride of `bgra` (may exceed width*4 due to DXGI alignment).
// Returns false if the file couldn't be written.
bool WriteBmp(const std::string& path, int width, int height,
              int rowPitch, const uint8_t* bgra);
