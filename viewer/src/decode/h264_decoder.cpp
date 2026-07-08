#include "decode/h264_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
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
    AVFrame* swFrame = nullptr; // staging buffer for hw→sw download
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int swsWidth = 0;
    int swsHeight = 0;
    AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;

    AVBufferRef* hwDeviceCtx = nullptr;
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (swFrame) av_frame_free(&swFrame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    }
};

namespace {
// Negotiates the surface format with the decoder. We get called once after
// the decoder parses SPS/PPS and knows resolution. Returning hwPixFmt tells
// libavcodec "yes, we have a hwaccel binding for that — go ahead and decode
// onto a D3D11 texture". Returning the first software entry (typically
// AV_PIX_FMT_YUV420P) takes the software path instead.
AVPixelFormat getFormatCallback(AVCodecContext* ctx, const AVPixelFormat* pixFmts) {
    auto* impl = static_cast<H264Decoder::Impl*>(ctx->opaque);
    if (impl && impl->hwPixFmt != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == impl->hwPixFmt) return *p;
        }
    }
    return pixFmts[0];
}
} // namespace

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::Init(std::string* mode, std::string* errorOut) {
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
    // LOW_DELAY: tell the decoder "don't buffer for B-frame reordering".
    // The encoder is configured with max_b_frames=0 so there's nothing to
    // reorder, but ffmpeg's H.264 decoder still holds 1-2 frames by
    // default in case B-frames appear. That's a hidden 8-33ms of latency
    // per frame period that we can't afford in a remote-desktop pipeline.
    impl_->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    impl_->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Frame-parallel threading in the software H.264 decoder holds
    // (thread_count) frames worth of latency. Single-thread keeps
    // send_packet -> receive_frame as tight as possible. On the D3D11VA
    // path threads aren't used at all anyway, so this is a no-op there.
    impl_->ctx->thread_count = 1;
    impl_->ctx->thread_type = 0;
    // Signal explicitly: our stream never reorders, decoder shouldn't
    // wait to see whether a later frame comes with a lower POC.
    impl_->ctx->has_b_frames = 0;

    // Try D3D11VA. If anything goes wrong here we silently keep
    // hwDeviceCtx == nullptr and fall through to software decode.
    AVBufferRef* hwCtx = nullptr;
    int hwErr = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                       nullptr, nullptr, 0);
    if (hwErr >= 0) {
        impl_->hwDeviceCtx = hwCtx;
        impl_->hwPixFmt = AV_PIX_FMT_D3D11;
        impl_->ctx->hw_device_ctx = av_buffer_ref(hwCtx);
        impl_->ctx->opaque = impl_.get();
        impl_->ctx->get_format = getFormatCallback;
        impl_->swFrame = av_frame_alloc();
        if (!impl_->swFrame) {
            if (errorOut) *errorOut = "av_frame_alloc(swFrame) failed";
            return false;
        }
    }

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
    if (mode) *mode = impl_->hwDeviceCtx ? "d3d11va" : "software";
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

        AVFrame* src = impl_->frame;
        AVPixelFormat srcFmt = static_cast<AVPixelFormat>(impl_->frame->format);
        const bool wasOnGpu = (srcFmt == impl_->hwPixFmt && impl_->hwDeviceCtx);
        if (wasOnGpu) {
            // GPU-decoded — pull down to CPU memory for swscale.
            // The downloaded format is typically NV12 (D3D11VA's surface
            // format on most GPUs). sws_scale handles NV12→BGRA natively.
            err = av_hwframe_transfer_data(impl_->swFrame, impl_->frame, 0);
            if (err < 0) {
                av_frame_unref(impl_->frame);
                if (errorOut) *errorOut = avErr("av_hwframe_transfer_data", err);
                return false;
            }
            src = impl_->swFrame;
            srcFmt = static_cast<AVPixelFormat>(impl_->swFrame->format);
        }

        const int w = src->width;
        const int h = src->height;

        if (!impl_->sws || impl_->swsWidth != w || impl_->swsHeight != h ||
            impl_->swsSrcFmt != srcFmt) {
            if (impl_->sws) sws_freeContext(impl_->sws);
            impl_->sws = sws_getContext(
                w, h, srcFmt,
                w, h, AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!impl_->sws) {
                av_frame_unref(impl_->frame);
                if (wasOnGpu) av_frame_unref(impl_->swFrame);
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
        out.pixels = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(out.rowBytes) * h);

        std::uint8_t* dstSlices[1] = { out.pixels->data() };
        int dstStrides[1] = { out.rowBytes };
        sws_scale(impl_->sws, src->data, src->linesize, 0, h,
                  dstSlices, dstStrides);

        outFrames->push_back(std::move(out));
        av_frame_unref(impl_->frame);
        if (wasOnGpu) av_frame_unref(impl_->swFrame);
    }
    return true;
}
