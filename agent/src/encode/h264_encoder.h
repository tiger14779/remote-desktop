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
        // Encoder OUTPUT dimensions (what the H.264 stream actually contains
        // and what the viewer sees). Should be even (H.264 + YUV420
        // requirement).
        int width = 1920;
        int height = 1080;
        // Source BGRA dimensions. If non-zero AND different from width/height,
        // sws_scale downsamples from src to dst as part of the BGRA→YUV
        // conversion — free downscale. If 0, treated as same as width/height
        // (no scale).
        int srcWidth = 0;
        int srcHeight = 0;
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
    //
    // If forceKeyframe is true, this frame is encoded as an IDR (also resets
    // any intra-refresh cycle). Used to resync the decoder after a long idle
    // gap during which the agent skipped sending frames.
    bool EncodeFrame(const uint8_t* bgra, int rowPitch, int64_t pts,
                     bool forceKeyframe,
                     std::vector<std::vector<uint8_t>>* outPackets,
                     std::string* errorOut);

    // Drain any frames the encoder is still buffering.
    bool Flush(std::vector<std::vector<uint8_t>>* outPackets, std::string* errorOut);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
