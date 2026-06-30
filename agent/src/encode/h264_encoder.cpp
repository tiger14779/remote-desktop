#include "encode/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string avErr(const std::string& what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    std::stringstream ss;
    ss << what << ": " << buf;
    return ss.str();
}

// Per-encoder tuning. Anything not listed falls back to encoder defaults.
//
// Intra-refresh (rolling I-stripe instead of periodic IDR keyframes):
//   - Eliminates the periodic bandwidth spike from full IDR frames
//   - Each frame intra-codes ~1/gop_size of the picture; after gop_size
//     frames, the whole picture has been refreshed
//   - Standard technique for cloud gaming / low-latency screen streaming
//   - Supported on libx264 and h264_nvenc; h264_qsv via int_ref_type; not
//     exposed cleanly on h264_amf or h264_mf
void applyLowLatencyTuning(AVCodecContext* ctx, const char* encoderName) {
    const std::string name = encoderName;
    if (name == "libx264") {
        // superfast keeps CABAC + 8x8 transform (sharp text), but cuts motion
        // estimation cost enough that 4K @ 30fps fits in real-time on a mid
        // tier CPU. "fast" was too heavy — encode latency was alone busting
        // the 33ms frame budget on user-reported hardware, which queued up
        // into ~1s of click-to-display lag.
        av_opt_set(ctx->priv_data, "preset", "superfast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "profile", "high", 0);

        // VBV: let the encoder BURST to 4x average bitrate for sudden
        // scene changes (cmd → txt full-window, opening fullscreen apps,
        // etc.). Without these, x264's default rate controller spreads a
        // single big I-frame across the next ~30 frames worth of budget,
        // and the viewer sees a multi-second "catch up" lag after every
        // major screen change. Buffer is 2s so the controller has room to
        // amortize without strangling individual frames.
        //
        // sliced-threads + sync-lookahead=0 + rc-lookahead=0 keep the
        // pipeline depth at 1 frame; without these, x264's threading model
        // can still buffer a few frames internally even with zerolatency.
        char x264Params[256];
        // ctx->bit_rate is bytes/sec; vbv-maxrate/bufsize want kbits/sec.
        const int avgKbps = static_cast<int>(ctx->bit_rate / 1000);
        const int vbvMax = avgKbps * 4;
        const int vbvBuf = avgKbps * 2;
        std::snprintf(x264Params, sizeof(x264Params),
                      "scenecut=0:repeat-headers=1:intra-refresh=1:ref=1:bframes=0"
                      ":sliced-threads=1:sync-lookahead=0:rc-lookahead=0"
                      ":vbv-maxrate=%d:vbv-bufsize=%d",
                      vbvMax, vbvBuf);
        av_opt_set(ctx->priv_data, "x264-params", x264Params, 0);
    } else if (name == "h264_nvenc") {
        // p1 = fastest preset; on NVENC silicon the quality cost is small
        // compared to the latency win, and screen content doesn't benefit
        // from heavy motion-estimation anyway.
        av_opt_set(ctx->priv_data, "preset", "p1", 0);
        // ull = ultra-low-latency: disables lookahead and B-frames implicitly,
        // single-pass rate control, packets emitted as soon as encoded.
        av_opt_set(ctx->priv_data, "tune", "ull", 0);
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        av_opt_set(ctx->priv_data, "delay", "0", 0);
        // CBR keeps each frame within a tight bit budget — pairs naturally
        // with intra-refresh (no big IDR spikes to fit in).
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx->priv_data, "intra-refresh", "1", 0);
        // forced-idr=1 makes frame->pict_type=I produce a true IDR; without it
        // NVENC may ignore the request mid-intra-refresh cycle.
        av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
        // VBV bufsize ≈ 1 frame's worth, so HRD-compliant viewers don't
        // pre-buffer half a second before starting playback. With tune=ull
        // + delay=0 + intra-refresh, per-frame bit budget stays close to
        // average; the encoder doesn't need a large smoothing buffer.
        const int avgKbps = static_cast<int>(ctx->bit_rate / 1000);
        const int fpsGuess = ctx->framerate.num > 0 ? ctx->framerate.num : 60;
        ctx->rc_buffer_size = (avgKbps * 1000) / fpsGuess;  // 1 frame worth
        ctx->rc_max_rate    = ctx->bit_rate;
    } else if (name == "h264_amf") {
        av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(ctx->priv_data, "quality", "speed", 0);
    } else if (name == "h264_qsv") {
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "int_ref_type", "vertical", 0); // vertical stripe intra-refresh
        av_opt_set(ctx->priv_data, "int_ref_cycle_size", "30", 0);
    } else if (name == "h264_mf") {
        av_opt_set(ctx->priv_data, "scenario", "live_streaming", 0);
        av_opt_set(ctx->priv_data, "rate_control", "cbr", 0);
        // h264_mf doesn't expose intra-refresh; it'll fall back to IDR
        // keyframes at ctx->gop_size cadence. Quality is the worst of the
        // bunch — install ffmpeg[x264] in vcpkg to unlock libx264.
    }
}
} // namespace

struct H264Encoder::Impl {
    Config cfg;
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int64_t nextPts = 0;

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::Init(const Config& cfg, std::string* chosenEncoder, std::string* errorOut) {
    impl_->cfg = cfg;

    // Priority: hardware encoders first (lowest latency, lowest CPU), then
    // libx264 (best quality at low bitrate), then h264_mf (worst, last resort).
    // h264_mf used to be 4th here, but its screen-content quality is so bad
    // that we'd rather burn a bit of CPU on libx264 when no hwenc is around.
    static const std::array<const char*, 5> kPriority = {
        "h264_nvenc", "h264_amf", "h264_qsv", "libx264", "h264_mf",
    };

    // Tells the operator exactly which encoders are missing from the build
    // and which ones tried-but-failed-to-open. This is the cheapest possible
    // answer to "why did it pick h264_mf?" — you read the stderr log.
    std::cerr << "[encoder] probing in order: nvenc, amf, qsv, libx264, mf\n";
    std::string lastErr;
    for (const char* name : kPriority) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) {
            std::cerr << "[encoder] " << name << ": not built into this libavcodec\n";
            lastErr = std::string(name) + ": not built";
            continue;
        }
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            std::cerr << "[encoder] " << name << ": avcodec_alloc_context3 failed\n";
            lastErr = std::string(name) + ": alloc failed";
            continue;
        }
        ctx->width = cfg.width;
        ctx->height = cfg.height;
        ctx->time_base = {1, cfg.fps};
        ctx->framerate = {cfg.fps, 1};
        ctx->pix_fmt = AV_PIX_FMT_YUV420P; // every h264 encoder accepts this
        ctx->bit_rate = int64_t(cfg.bitrateKbps) * 1000;
        ctx->gop_size = cfg.gopSize;
        ctx->max_b_frames = 0; // low latency
        ctx->thread_count = 0; // auto

        applyLowLatencyTuning(ctx, name);

        int err = avcodec_open2(ctx, codec, nullptr);
        if (err < 0) {
            std::cerr << "[encoder] " << name << ": "
                      << avErr("open", err) << "\n";
            lastErr = avErr(std::string(name) + ": open", err);
            avcodec_free_context(&ctx);
            continue;
        }
        std::cerr << "[encoder] " << name << ": OK — using this one\n";

        impl_->codec = codec;
        impl_->ctx = ctx;
        if (chosenEncoder) *chosenEncoder = name;
        break;
    }
    if (!impl_->ctx) {
        if (errorOut) *errorOut = "no usable H.264 encoder. last error: " + lastErr;
        return false;
    }

    impl_->frame = av_frame_alloc();
    if (!impl_->frame) {
        if (errorOut) *errorOut = "av_frame_alloc failed";
        return false;
    }
    impl_->frame->format = impl_->ctx->pix_fmt;
    impl_->frame->width = impl_->ctx->width;
    impl_->frame->height = impl_->ctx->height;
    int err = av_frame_get_buffer(impl_->frame, 0);
    if (err < 0) {
        if (errorOut) *errorOut = avErr("av_frame_get_buffer", err);
        return false;
    }

    impl_->pkt = av_packet_alloc();
    if (!impl_->pkt) {
        if (errorOut) *errorOut = "av_packet_alloc failed";
        return false;
    }

    const int srcW = cfg.srcWidth  > 0 ? cfg.srcWidth  : cfg.width;
    const int srcH = cfg.srcHeight > 0 ? cfg.srcHeight : cfg.height;
    // SWS_AREA gives much sharper text on big downscales (4K → 2K) than
    // SWS_BILINEAR; for no-scale or upscale paths it's equivalent.
    const int swsFlags = (srcW != cfg.width || srcH != cfg.height)
                             ? SWS_AREA
                             : SWS_BILINEAR;
    impl_->sws = sws_getContext(
        srcW, srcH, AV_PIX_FMT_BGRA,
        cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
        swsFlags, nullptr, nullptr, nullptr);
    if (!impl_->sws) {
        if (errorOut) *errorOut = "sws_getContext failed";
        return false;
    }

    return true;
}

bool H264Encoder::EncodeFrame(const uint8_t* bgra, int rowPitch, int64_t pts,
                              bool forceKeyframe,
                              std::vector<std::vector<uint8_t>>* outPackets,
                              std::string* errorOut) {
    if (!impl_->ctx) {
        if (errorOut) *errorOut = "encoder not initialised";
        return false;
    }

    int err = av_frame_make_writable(impl_->frame);
    if (err < 0) {
        if (errorOut) *errorOut = avErr("av_frame_make_writable", err);
        return false;
    }

    const uint8_t* srcSlices[1] = { bgra };
    int srcStrides[1] = { rowPitch };
    // srcSliceH is in SOURCE rows, which may differ from the encoder output
    // height when downscale is in effect (e.g. 4K → 2K source = 2160 rows).
    const int srcH = impl_->cfg.srcHeight > 0 ? impl_->cfg.srcHeight
                                              : impl_->cfg.height;
    sws_scale(impl_->sws, srcSlices, srcStrides, 0, srcH,
              impl_->frame->data, impl_->frame->linesize);

    impl_->frame->pts = pts;
    // Setting pict_type = I is what the encoder actually checks to produce
    // an IDR; AVFrame::key_frame was deprecated in FFmpeg 6.x.
    impl_->frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    err = avcodec_send_frame(impl_->ctx, impl_->frame);
    if (err < 0) {
        if (errorOut) *errorOut = avErr("avcodec_send_frame", err);
        return false;
    }

    while (true) {
        err = avcodec_receive_packet(impl_->ctx, impl_->pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if (errorOut) *errorOut = avErr("avcodec_receive_packet", err);
            return false;
        }
        outPackets->emplace_back(impl_->pkt->data, impl_->pkt->data + impl_->pkt->size);
        av_packet_unref(impl_->pkt);
    }
    return true;
}

bool H264Encoder::Flush(std::vector<std::vector<uint8_t>>* outPackets, std::string* errorOut) {
    if (!impl_->ctx) return true;
    int err = avcodec_send_frame(impl_->ctx, nullptr);
    if (err < 0 && err != AVERROR_EOF) {
        if (errorOut) *errorOut = avErr("avcodec_send_frame(flush)", err);
        return false;
    }
    while (true) {
        err = avcodec_receive_packet(impl_->ctx, impl_->pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if (errorOut) *errorOut = avErr("avcodec_receive_packet(flush)", err);
            return false;
        }
        outPackets->emplace_back(impl_->pkt->data, impl_->pkt->data + impl_->pkt->size);
        av_packet_unref(impl_->pkt);
    }
    return true;
}
