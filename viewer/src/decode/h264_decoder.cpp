#include "decode/h264_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sstream>

namespace {
std::string avErr(const std::string& what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    std::stringstream ss;
    ss << what << ": " << buf;
    return ss.str();
}
} // namespace

struct H264Decoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int swsWidth = 0;
    int swsHeight = 0;
    AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::Init(std::string* errorOut) {
    impl_->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!impl_->codec) {
        if (errorOut) *errorOut = "libavcodec lacks the H.264 decoder";
        return false;
    }
    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->ctx) {
        if (errorOut) *errorOut = "avcodec_alloc_context3 failed";
        return false;
    }
    impl_->ctx->thread_count = 0; // auto
    int err = avcodec_open2(impl_->ctx, impl_->codec, nullptr);
    if (err < 0) {
        if (errorOut) *errorOut = avErr("avcodec_open2", err);
        return false;
    }
    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        if (errorOut) *errorOut = "av_frame_alloc / av_packet_alloc failed";
        return false;
    }
    return true;
}

bool H264Decoder::DecodePacket(const std::uint8_t* data, std::size_t size,
                               std::vector<DecodedFrame>* outFrames,
                               std::string* errorOut) {
    if (!impl_->ctx) {
        if (errorOut) *errorOut = "decoder not initialised";
        return false;
    }
    impl_->pkt->data = const_cast<std::uint8_t*>(data);
    impl_->pkt->size = static_cast<int>(size);

    int err = avcodec_send_packet(impl_->ctx, impl_->pkt);
    if (err < 0) {
        if (errorOut) *errorOut = avErr("avcodec_send_packet", err);
        return false;
    }

    while (true) {
        err = avcodec_receive_frame(impl_->ctx, impl_->frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if (errorOut) *errorOut = avErr("avcodec_receive_frame", err);
            return false;
        }

        const int w = impl_->frame->width;
        const int h = impl_->frame->height;
        const auto srcFmt = static_cast<AVPixelFormat>(impl_->frame->format);

        // Lazily create sws context, rebuild on resolution/format change.
        if (!impl_->sws || impl_->swsWidth != w || impl_->swsHeight != h ||
            impl_->swsSrcFmt != srcFmt) {
            if (impl_->sws) sws_freeContext(impl_->sws);
            impl_->sws = sws_getContext(
                w, h, srcFmt,
                w, h, AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!impl_->sws) {
                if (errorOut) *errorOut = "sws_getContext failed";
                return false;
            }
            impl_->swsWidth = w;
            impl_->swsHeight = h;
            impl_->swsSrcFmt = srcFmt;
        }

        DecodedFrame out;
        out.width = w;
        out.height = h;
        out.rowBytes = w * 4;
        out.pixels.resize(static_cast<std::size_t>(out.rowBytes) * h);

        std::uint8_t* dstSlices[1] = { out.pixels.data() };
        int dstStrides[1] = { out.rowBytes };
        sws_scale(impl_->sws, impl_->frame->data, impl_->frame->linesize, 0, h,
                  dstSlices, dstStrides);

        outFrames->push_back(std::move(out));
        av_frame_unref(impl_->frame);
    }
    return true;
}
