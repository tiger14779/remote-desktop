// Phase 3 smoke: feed a raw Annex-B .h264 file through H264Decoder. The file
// may contain many access units concatenated, so we use libavcodec's H.264
// parser to slice them. Prints frame count + final-frame dimensions.

#include "decode/h264_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: decode_test in.h264\n";
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::cerr << "open " << argv[1] << ": failed\n";
        return 1;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), data.size());

    H264Decoder dec;
    std::string err;
    std::string mode;
    if (!dec.Init(&mode, &err)) {
        std::cerr << "decoder init: " << err << "\n";
        return 2;
    }

    // Parser splits the raw Annex-B bytestream into one AU per call.
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* parserCtx = avcodec_alloc_context3(
        avcodec_find_decoder(AV_CODEC_ID_H264));
    avcodec_open2(parserCtx, parserCtx->codec, nullptr);

    std::size_t off = 0;
    int total = 0, lastW = 0, lastH = 0;
    while (off < data.size()) {
        std::uint8_t* out = nullptr;
        int outSize = 0;
        int used = av_parser_parse2(
            parser, parserCtx, &out, &outSize,
            data.data() + off, static_cast<int>(data.size() - off),
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            std::cerr << "parser error\n";
            break;
        }
        off += used;
        if (outSize > 0) {
            std::vector<H264Decoder::DecodedFrame> frames;
            if (!dec.DecodePacket(out, outSize, &frames, &err)) {
                std::cerr << "decode: " << err << "\n";
                break;
            }
            for (const auto& f : frames) {
                total++;
                lastW = f.width;
                lastH = f.height;
            }
        }
    }

    // Flush.
    std::vector<H264Decoder::DecodedFrame> trailing;
    dec.DecodePacket(nullptr, 0, &trailing, &err);
    for (const auto& f : trailing) {
        total++;
        lastW = f.width;
        lastH = f.height;
    }

    av_parser_close(parser);
    avcodec_free_context(&parserCtx);

    std::cout << "decoded " << total << " frame(s) via " << mode
              << ", last size " << lastW << "x" << lastH << "\n";
    return total > 0 ? 0 : 3;
}
