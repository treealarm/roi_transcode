#include "frame_input.h"

#include <cstring>
namespace sve {


using roiav::check;
using roiav::fail;

namespace {

AVCodecID codec_id_of(sve::InputCodec c)
{
    switch (c) {
    case sve::InputCodec::H264:
        return AV_CODEC_ID_H264;
    case sve::InputCodec::H265:
        return AV_CODEC_ID_HEVC;
    case sve::InputCodec::Jpeg:
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

AVPixelFormat FrameDecoder::choose_format(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    auto* self = static_cast<FrameDecoder*>(ctx->opaque);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (self && *p == self->m_hw_pix) {
            self->m_hardware_name = av_hwdevice_get_type_name(self->m_hw_type);
            return *p;
        }
    }
    // Not an error: the device is there but this stream is not something it decodes -- a
    // ten-bit profile on silicon that has none, most often. Software takes it, and
    // m_hardware_name stays empty so the caller can see that it did.
    return formats[0];
}

bool FrameDecoder::open_context(const AVCodec* dec, const uint8_t* extradata,
    int extradata_size, AVBufferRef* hw_device)
{
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

    if (hw_device) {
        // Which pixel format this device speaks for this decoder, asked of libavcodec rather
        // than hardcoded: the same question answers VA-API, QSV and CUDA, and a table would
        // be one more place to be wrong when a fourth arrives.
        const auto* device = reinterpret_cast<const AVHWDeviceContext*>(hw_device->data);
        m_hw_type = device->type;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) {
                break;
            }
            if (cfg->device_type == m_hw_type
                && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                m_hw_pix = cfg->pix_fmt;
                break;
            }
        }
        if (m_hw_pix != AV_PIX_FMT_NONE) {
            m_dec.p->hw_device_ctx = av_buffer_ref(hw_device);
            m_dec.p->opaque = this;
            m_dec.p->get_format = &FrameDecoder::choose_format;
        }
    }

    if (avcodec_open2(m_dec.p, dec, nullptr) < 0) {
        m_dec = roiav::CodecCtx{};
        m_hw_pix = AV_PIX_FMT_NONE;
        m_hw_type = AV_HWDEVICE_TYPE_NONE;
        return false;
    }
    return true;
}

void FrameDecoder::open(sve::InputCodec codec, const uint8_t* extradata, int extradata_size,
    AVBufferRef* hw_device)
{
    m_codec = codec;
    if (codec == sve::InputCodec::Raw) {
        return;
    }

    const AVCodecID id = codec_id_of(codec);
    const AVCodec* dec = avcodec_find_decoder(id);
    if (!dec) {
        fail("no decoder for the requested input codec");
    }
    m_decoder_name = dec->name;

    // Two attempts at most, and the second only where the first asked for hardware: opening
    // a decoder on a device that turns out to have no path for this codec must cost the
    // caller a software decode, not the session.
    if (!open_context(dec, extradata, extradata_size, hw_device)
        && (!hw_device || !open_context(dec, extradata, extradata_size, nullptr))) {
        check(AVERROR(EINVAL), "open decoder");
    }

    // A caller promising whole frames is usually right, but an Annex-B stream
    // arriving in arbitrary chunks is common enough that going without a parser
    // would fail quietly and confusingly. JPEG is self-delimiting.
    if (codec == sve::InputCodec::H264 || codec == sve::InputCodec::H265) {
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
    if (m_codec == sve::InputCodec::Raw) {
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
    if (m_codec == sve::InputCodec::Raw || !m_dec.p) {
        return;
    }
    check(avcodec_send_packet(m_dec.p, nullptr), "flush decoder");
    receive(cb);
}

} // namespace sve
