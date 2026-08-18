#pragma once

// Turns what a caller pushes into AVFrames. Raw planes are wrapped without a
// copy; H.264, H.265 and JPEG go through a decoder.

#include "av_raii.h"
#include "roi/session.h"

#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace roi {

class FrameDecoder {
public:
    using FrameCb = std::function<void(AVFrame*)>;

    // `extradata` may be null; for H.264/H.265 in AVCC/HVCC form it is required
    // for the decoder to make sense of the first access unit.
    void open(roi::InputCodec codec, const uint8_t* extradata, int extradata_size);

    // Feeds one buffer. Calls `cb` for every frame that comes out -- usually one,
    // sometimes none while the decoder builds up state.
    void push(const uint8_t* data, size_t size, int64_t pts, const FrameCb& cb);
    // Drains frames the decoder is still holding.
    void flush(const FrameCb& cb);

private:
    void receive(const FrameCb& cb);

    roi::InputCodec m_codec = roi::InputCodec::Raw;
    roiav::CodecCtx m_dec;
    AVCodecParserContext* m_parser = nullptr;
    roiav::Frame m_frame;
    roiav::Packet m_pkt;

public:
    ~FrameDecoder();
    FrameDecoder() = default;
    FrameDecoder(const FrameDecoder&) = delete;
    FrameDecoder& operator=(const FrameDecoder&) = delete;
};

} // namespace roi
