#include "picture_input.h"

#include <cstring>
namespace ta_roienc {


using roiav::check;
using roiav::fail;

namespace {

AVCodecID codec_id_of(ta_roienc::SourceCodec c)
{
    switch (c) {
    case ta_roienc::SourceCodec::H264:
        return AV_CODEC_ID_H264;
    case ta_roienc::SourceCodec::H265:
        return AV_CODEC_ID_HEVC;
    case ta_roienc::SourceCodec::Jpeg:
        return AV_CODEC_ID_MJPEG;
    default:
        return AV_CODEC_ID_NONE;
    }
}

/// Whether `extradata` is a length-prefixed configuration record -- AVCC for H.264, HVCC for
/// H.265 -- rather than Annex-B parameter sets.
///
/// The difference decides whether a parser belongs in front of the decoder, and getting it wrong
/// is silent in the worst way: an Annex-B parser fed length-prefixed access units finds no start
/// codes, emits nothing, and the stream decodes to a black picture with not one line in the log
/// to say why. Both records begin with configurationVersion, which is 1; Annex-B parameter sets
/// begin with a start code, whose first byte is 0. That is the whole test, and it is the same one
/// libavformat makes.
bool is_length_prefixed(const uint8_t* extradata, int extradata_size)
{
    if (!extradata || extradata_size < 4) {
        return false;
    }
    return extradata[0] == 1;
}

} // namespace

PictureDecoder::~PictureDecoder()
{
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
}

AVPixelFormat PictureDecoder::choose_format(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    auto* self = static_cast<PictureDecoder*>(ctx->opaque);
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

bool PictureDecoder::open_context(const AVCodec* dec, const uint8_t* extradata,
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
            m_dec.p->get_format = &PictureDecoder::choose_format;
            // Surfaces beyond what the decoder needs for reference frames, for whoever is
            // holding the output: the encoder on the transcode path, or a caller reading
            // pixels back on the analytics one. libavcodec's default of a handful is
            // enough for neither -- measured on two 2.5K streams through one VA-API
            // device, where av_hwframe_transfer_data stops being able to sync and the
            // decode fails after minutes of working. Costs surface memory and nothing
            // else, so it is sized for the case that fails rather than the one that does
            // not.
            m_dec.p->extra_hw_frames = 32;
        }
    }

    if (avcodec_open2(m_dec.p, dec, nullptr) < 0) {
        m_dec = roiav::CodingCtx{};
        m_hw_pix = AV_PIX_FMT_NONE;
        m_hw_type = AV_HWDEVICE_TYPE_NONE;
        return false;
    }
    return true;
}

void PictureDecoder::open(ta_roienc::SourceCodec codec, const uint8_t* extradata, int extradata_size,
    AVBufferRef* hw_device, Framing framing)
{
    m_codec = codec;
    if (codec == ta_roienc::SourceCodec::Raw) {
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

    // An Annex-B stream arriving in arbitrary chunks is common enough that going without a
    // parser would fail quietly and confusingly, so it is what a caller gets by default.
    // JPEG is self-delimiting.
    //
    // Two callers do not need it, and for them it is a cost rather than a safety net -- see
    // Framing on the delay it adds. A caller that says its buffers are whole access units is
    // taken at its word; libavformat, which is where both of ours get theirs, splits them
    // already.
    //
    // Length-prefixed input is the other exception, and that one is not the caller's to
    // choose, because a parser is not merely unnecessary there but actively wrong -- see
    // is_length_prefixed. libavcodec reads the configuration record out of extradata and
    // splits the access units itself, so the packets go straight through.
    if ((codec == ta_roienc::SourceCodec::H264 || codec == ta_roienc::SourceCodec::H265)
        && framing == Framing::Arbitrary
        && !is_length_prefixed(extradata, extradata_size)) {
        m_parser = av_parser_init(id);
    }
}

void PictureDecoder::receive(const PictureCb& cb)
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

void PictureDecoder::push(
    const uint8_t* data, size_t size, int64_t pts, const PictureCb& cb)
{
    if (m_codec == ta_roienc::SourceCodec::Raw) {
        fail("PictureDecoder::push called on a raw session");
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

void PictureDecoder::flush(const PictureCb& cb)
{
    if (m_codec == ta_roienc::SourceCodec::Raw || !m_dec.p) {
        return;
    }
    check(avcodec_send_packet(m_dec.p, nullptr), "flush decoder");
    receive(cb);
}

} // namespace ta_roienc
