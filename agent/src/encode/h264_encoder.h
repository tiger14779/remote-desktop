#pragma once

// H.264 encoder wrapping libavcodec.
//
// Init() tries a priority-ordered list of encoder names and uses the first one
// that opens successfully:
//
//   h264_nvenc → h264_amf → h264_qsv → h264_mf → libx264
//
// Hardware encoders give better latency/CPU; libx264 is the portable fallback.
// The caller learns which encoder was actually selected via `chosenEncoder`.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class H264Encoder {
public:
    struct Config {
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int bitrateKbps = 4000;
        int gopSize = 60; // keyframe every N frames (~2s @ 30fps)
    };

    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool Init(const Config& cfg, std::string* chosenEncoder, std::string* errorOut);

    // Encode one BGRA frame. May produce zero, one, or several Annex-B NAL
    // packets, appended to outPackets in order.
    bool EncodeFrame(const uint8_t* bgra, int rowPitch, int64_t pts,
                     std::vector<std::vector<uint8_t>>* outPackets,
                     std::string* errorOut);

    // Drain any frames the encoder is still buffering.
    bool Flush(std::vector<std::vector<uint8_t>>* outPackets, std::string* errorOut);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
