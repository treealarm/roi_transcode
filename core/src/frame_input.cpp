#include "frame_input.h"

#include <cstring>

using roiav::check;
using roiav::fail;

namespace {

AVCodecID codec_id_of(roi::InputCodec c)
{
    switch (c) {
    case roi::InputCodec::H264:
        return AV_CODEC_ID_H264;
    case roi::InputCodec::H265:
        return AV_CODEC_ID_HEVC;
    case roi::InputCodec::Jpeg:
        return AV_CODEC_ID_MJPEG;
    default:
        return AV_CODEC_ID_NONE;
    }
}

} // namespace

FrameDecoder::~FrameDecoder()
{
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
}

void FrameDecoder::open(
    roi::InputCodec codec, const uint8_t* extradata, int extradata_size)
{
    m_codec = codec;
    if (codec == roi::InputCodec::Raw) {
        return;
    }

    const AVCodecID id = codec_id_of(codec);
    const AVCodec* dec = avcodec_find_decoder(id);
    if (!dec) {
        fail("no decoder for the requested input codec");
    }
    m_dec.p = avcodec_alloc_context3(dec);
    if (!m_dec.p) {
        fail("alloc decoder");
    }
    if (extradata && extradata_size > 0) {
        m_dec.p->extradata = static_cast<uint8_t*>(
            av_mallocz(static_cast<size_t>(extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_dec.p->extradata) {
            fail("alloc extradata");
        }
        std::memcpy(m_dec.p->extradata, extradata, static_cast<size_t>(extradata_size));
        m_dec.p->extradata_size = extradata_size;
    }
    check(avcodec_open2(m_dec.p, dec, nullptr), "open decoder");

    // A caller promising whole frames is usually right, but an Annex-B stream
    // arriving in arbitrary chunks is common enough that going without a parser
    // would fail quietly and confusingly. JPEG is self-delimiting.
    if (codec == roi::InputCodec::H264 || codec == roi::InputCodec::H265) {
        m_parser = av_parser_init(id);
    }
}

void FrameDecoder::receive(const FrameCb& cb)
{
    for (;;) {
        const int r = avcodec_receive_frame(m_dec.p, m_frame.p);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return;
        }
        check(r, "receive frame");
        cb(m_frame.p);
        av_frame_unref(m_frame.p);
    }
}

void FrameDecoder::push(
    const uint8_t* data, size_t size, int64_t pts, const FrameCb& cb)
{
    if (m_codec == roi::InputCodec::Raw) {
        fail("FrameDecoder::push called on a raw session");
    }
    if (!data || size == 0) {
        return;
    }

    if (!m_parser) {
        av_packet_unref(m_pkt.p);
        m_pkt.p->data = const_cast<uint8_t*>(data);
        m_pkt.p->size = static_cast<int>(size);
        m_pkt.p->pts = pts;
        m_pkt.p->dts = pts;
        check(avcodec_send_packet(m_dec.p, m_pkt.p), "send packet");
        m_pkt.p->data = nullptr;
        m_pkt.p->size = 0;
        receive(cb);
        return;
    }

    const uint8_t* in = data;
    size_t left = size;
    while (left > 0) {
        uint8_t* out = nullptr;
        int out_size = 0;
        const int used = av_parser_parse2(m_parser, m_dec.p, &out, &out_size, in,
            static_cast<int>(left), pts, pts, 0);
        if (used < 0) {
            fail("parser failed");
        }
        in += used;
        left -= static_cast<size_t>(used);
        if (out_size <= 0) {
            continue;
        }
        av_packet_unref(m_pkt.p);
        m_pkt.p->data = out;
        m_pkt.p->size = out_size;
        m_pkt.p->pts = pts;
        m_pkt.p->dts = pts;
        check(avcodec_send_packet(m_dec.p, m_pkt.p), "send packet");
        m_pkt.p->data = nullptr;
        m_pkt.p->size = 0;
        receive(cb);
    }
}

void FrameDecoder::flush(const FrameCb& cb)
{
    if (m_codec == roi::InputCodec::Raw || !m_dec.p) {
        return;
    }
    check(avcodec_send_packet(m_dec.p, nullptr), "flush decoder");
    receive(cb);
}
