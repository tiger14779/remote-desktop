#include "util/bmp_writer.h"

#include <cstdint>
#include <fstream>

bool WriteBmp(const std::string& path, int width, int height,
              int rowPitch, const uint8_t* bgra) {
    if (width <= 0 || height <= 0 || rowPitch < width * 4 || bgra == nullptr) {
        return false;
    }

    const uint32_t imageSize = static_cast<uint32_t>(width) *
                               static_cast<uint32_t>(height) * 4;
    const uint32_t fileSize = 14 + 40 + imageSize;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    // Write headers field-by-field to avoid any padding pitfalls across
    // compilers — BMP is a tiny format, this is fine.
    auto put16 = [&](uint16_t v) {
        char b[2] = { static_cast<char>(v & 0xFF),
                      static_cast<char>((v >> 8) & 0xFF) };
        out.write(b, 2);
    };
    auto put32 = [&](uint32_t v) {
        char b[4] = { static_cast<char>(v & 0xFF),
                      static_cast<char>((v >> 8) & 0xFF),
                      static_cast<char>((v >> 16) & 0xFF),
                      static_cast<char>((v >> 24) & 0xFF) };
        out.write(b, 4);
    };
    auto put32s = [&](int32_t v) { put32(static_cast<uint32_t>(v)); };

    // BITMAPFILEHEADER (14 bytes)
    out.put('B'); out.put('M');
    put32(fileSize);
    put16(0); put16(0);
    put32(54);

    // BITMAPINFOHEADER (40 bytes)
    put32(40);
    put32s(width);
    put32s(-height); // negative height => top-down rows
    put16(1);
    put16(32);
    put32(0); // BI_RGB
    put32(imageSize);
    put32s(0); put32s(0);
    put32(0); put32(0);

    // Pixel data: repack rows to remove DXGI's row padding.
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        out.write(reinterpret_cast<const char*>(bgra + static_cast<size_t>(y) * rowPitch),
                  static_cast<std::streamsize>(rowBytes));
    }
    return out.good();
}
