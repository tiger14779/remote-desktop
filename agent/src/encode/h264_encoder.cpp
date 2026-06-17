#include "encode/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <array>
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
void applyLowLatencyTuning(AVCodecContext* ctx, const char* encoderName) {
    const std::string name = encoderName;
    if (name == "libx264") {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "x264-params", "scenecut=0:repeat-headers=1", 0);
    } else if (name == "h264_nvenc") {
        av_opt_set(ctx->priv_data, "preset", "p4", 0);
        av_opt_set(ctx->priv_data, "tune", "ll", 0);   // low-latency
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
    } else if (name == "h264_amf") {
        av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(ctx->priv_data, "quality", "speed", 0);
    } else if (name == "h264_qsv") {
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
    } else if (name == "h264_mf") {
        av_opt_set(ctx->priv_data, "scenario", "live_streaming", 0);
        av_opt_set(ctx->priv_data, "rate_control", "cbr", 0);
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

    static const std::array<const char*, 5> kPriority = {
        "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", "libx264",
    };

    std::string lastErr;
    for (const char* name : kPriority) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) {
            lastErr = std::string(name) + ": not built into this libavcodec";
            continue;
        }
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            lastErr = std::string(name) + ": avcodec_alloc_context3 failed";
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
            lastErr = avErr(std::string(name) + ": avcodec_open2", err);
            avcodec_free_context(&ctx);
            continue;
        }

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

    impl_->sws = sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_BGRA,
        cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws) {
        if (errorOut) *errorOut = "sws_getContext failed";
        return false;
    }

    return true;
}

bool H264Encoder::EncodeFrame(const uint8_t* bgra, int rowPitch, int64_t pts,
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
    sws_scale(impl_->sws, srcSlices, srcStrides, 0, impl_->cfg.height,
              impl_->frame->data, impl_->frame->linesize);

    impl_->frame->pts = pts;

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
