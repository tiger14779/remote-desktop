#pragma once

// H.264 software decoder via libavcodec. Output is BGRA8 tightly packed (one
// byte per channel; pixel stride is width * 4). On little-endian this matches
// QImage::Format_RGB32 byte layout, so the Qt side can wrap the buffer
// directly without another copy.
//
// The agent guarantees one complete access unit per VIDEO_FRAME message, so
// no AV parser is needed — each DecodePacket call produces zero or one frames.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class H264Decoder {
public:
    struct DecodedFrame {
        int width = 0;
        int height = 0;
        int rowBytes = 0; // equals width * 4
        std::vector<std::uint8_t> pixels;
    };

    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Init tries D3D11VA hardware decode first; falls back to software if the
    // hwaccel isn't available (no GPU support, ffmpeg built without d3d11va,
    // missing driver, ...). On success, *mode reports the actual decoder
    // path: either "d3d11va" or "software".
    bool Init(std::string* mode, std::string* errorOut);

    bool DecodePacket(const std::uint8_t* data, std::size_t size,
                      std::vector<DecodedFrame>* outFrames,
                      std::string* errorOut);

    // Public so the static get_format callback can reach it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
