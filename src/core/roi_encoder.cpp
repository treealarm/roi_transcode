#include "roi_encoder.h"

#include "packet_sink.h"

#include <algorithm>
#include <cstring>
#include <format>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
namespace smartvideo {


using roiav::check;
using roiav::fail;

namespace {

void attach_roi(AVFrame* frame, const std::vector<Rect>& rects)
{
    av_frame_remove_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (rects.empty()) {
        return;
    }
    const int bytes =
        static_cast<int>(sizeof(AVRegionOfInterest) * rects.size());
    AVFrameSideData* sd = av_frame_new_side_data(
        frame, AV_FRAME_DATA_REGIONS_OF_INTEREST, bytes);
    if (!sd) {
        fail("av_frame_new_side_data ROI");
    }
    auto* roi = reinterpret_cast<AVRegionOfInterest*>(sd->data);
    for (size_t i = 0; i < rects.size(); ++i) {
        roi[i].self_size = sizeof(AVRegionOfInterest);
        roi[i].top = rects[i].top;
        roi[i].bottom = rects[i].bottom;
        roi[i].left = rects[i].left;
        roi[i].right = rects[i].right;
        const int qp = std::clamp(rects[i].qp_delta, -51, 51);
        roi[i].qoffset = AVRational{qp, 51};
    }
}

bool open_hw_device(roiav::Buffer& hw, AVHWDeviceType type, const char* device)
{
    av_buffer_unref(&hw.p);
    return av_hwdevice_ctx_create(&hw.p, type, device, nullptr, 0) >= 0;
}

bool init_vaapi_frames(roiav::Buffer& hw, roiav::Buffer& frames, int w, int h)
{
    av_buffer_unref(&frames.p);
    frames.p = av_hwframe_ctx_alloc(hw.p);
    if (!frames.p) {
        return false;
    }
    auto* ctx = reinterpret_cast<AVHWFramesContext*>(frames.p->data);
    ctx->format = AV_PIX_FMT_VAAPI;
    ctx->sw_format = AV_PIX_FMT_NV12;
    ctx->width = w;
    ctx->height = h;
    ctx->initial_pool_size = 24;
    return av_hwframe_ctx_init(frames.p) >= 0;
}

} // namespace

const char* RoiEncoder::label() const
{
    const bool h264 = m_codec == Codec::H264;
    switch (m_kind) {
    case Encoder::Qsv:
        return h264 ? "h264_qsv (Intel VPL mfxExtEncoderROI)"
                    : "hevc_qsv (Intel VPL mfxExtEncoderROI)";
    case Encoder::Vaapi:
        return h264 ? "h264_vaapi (Intel iHD VAEncROI)"
                    : "hevc_vaapi (Intel iHD VAEncROI)";
    case Encoder::Nvenc:
        return h264 ? "h264_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)"
                    : "hevc_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)";
    default:
        return h264 ? "libx264 (CPU fallback)" : "libx265 (CPU fallback)";
    }
}

namespace {

// A throwaway encode of a single frame, for backends that ask for it. Ubuntu's older iHD
// opens the QSV encoder happily and then returns empty packets, and only an actual encode
// tells the two apart -- which is why IEncoderBackend::NeedsProbe exists rather than the
// caller guessing which backends lie.
bool probe_one_frame(BackendSession& a)
{
    roiav::Frame f;
    f.p->format = a.sw_pix;
    f.p->width = a.enc.p->width;
    f.p->height = a.enc.p->height;
    if (av_frame_get_buffer(f.p, 32) < 0 || av_frame_make_writable(f.p) < 0) {
        return false;
    }
    const int h = a.enc.p->height;
    std::memset(f.p->data[0], 0, static_cast<size_t>(f.p->linesize[0]) * h);
    if (f.p->data[1]) {
        std::memset(f.p->data[1], 128,
            static_cast<size_t>(f.p->linesize[1]) * ((h + 1) / 2));
    }
    f.p->pts = 0;

    AVFrame* send = f.p;
    roiav::Frame hw;
    if (a.hw_upload) {
        if (av_hwframe_get_buffer(a.enc.p->hw_frames_ctx, hw.p, 0) < 0
            || av_hwframe_transfer_data(hw.p, f.p, 0) < 0) {
            return false;
        }
        hw.p->pts = 0;
        send = hw.p;
    }

    if (avcodec_send_frame(a.enc.p, send) < 0) {
        return false;
    }
    roiav::Packet pkt;
    int r = avcodec_receive_packet(a.enc.p, pkt.p);
    if (r == AVERROR(EAGAIN)) {
        if (avcodec_send_frame(a.enc.p, nullptr) < 0) {
            return false;
        }
        r = avcodec_receive_packet(a.enc.p, pkt.p);
    }
    return r >= 0;
}


// Allocates the context, lets the backend configure it, opens it, and honours a probe.
// Everything here is the same whichever backend answered -- what differs is inside
// Configure, which is the whole reason the interface exists.
// `probe` is what stops the recursion below: the reopen after a successful probe must not
// probe again, or the backend that asks for one never finishes opening.
bool open_with(BackendSession& out, const IEncoderBackend& backend,
    const AVCodec* codec, const EncoderParams& p, bool probe = true)
{
    out = BackendSession{};
    out.kind = backend.kind();
    out.enc.p = avcodec_alloc_context3(codec);
    if (!out.enc.p) {
        return false;
    }
    if (!backend.Configure(out, p)) {
        out = BackendSession{};
        return false;
    }
    if (avcodec_open2(out.enc.p, codec, nullptr) < 0) {
        out = BackendSession{};
        return false;
    }
    if (probe && backend.NeedsProbe()) {
        const bool ok = probe_one_frame(out);
        // The probe consumed the context either way: an encoder that has seen a frame
        // cannot be handed back as fresh.
        out = BackendSession{};
        if (!ok) {
            return false;
        }
        return open_with(out, backend, codec, p, false);
    }
    return true;
}

} // namespace

void RoiEncoder::open(const EncoderParams& params, PacketSink& sink)
{
    open_deferred(params);
    attach(sink);
}

void RoiEncoder::attach(PacketSink& sink)
{
    m_sink = &sink;
    m_sink->begin(m_enc.p);
}

EncoderCapabilities RoiEncoder::capabilities() const
{
    const IEncoderBackend* b = backend_for(m_kind);
    return b ? b->GetCapabilities(m_codec) : EncoderCapabilities{};
}

void RoiEncoder::open_deferred(const EncoderParams& params)
{
    const bool h264 = params.codec == Codec::H264;
    // Which backends to try, and their libavcodec names. The order is the registry's
    // (backend/common/backend_registry.cpp); the names differ only by the codec prefix.
    auto codec_name = [h264](Encoder kind) -> const char* {
        switch (kind) {
        case Encoder::Qsv:   return h264 ? "h264_qsv"   : "hevc_qsv";
        case Encoder::Vaapi: return h264 ? "h264_vaapi" : "hevc_vaapi";
        case Encoder::Nvenc: return h264 ? "h264_nvenc" : "hevc_nvenc";
        default:             return h264 ? "libx264"    : "libx265";
        }
    };

    BackendSession opened;
    std::string errors;
    bool got = false;
    for (const IEncoderBackend* backend : backend_chain(params.kind)) {
        const char* name = codec_name(backend->kind());
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec && backend->kind() == Encoder::Cpu) {
            // A build without libx264/libx265 may still have some other encoder for the
            // codec; better that than refusing to run at all.
            codec = avcodec_find_encoder(h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        }
        if (!codec) {
            errors += std::format(" {}: not in FFmpeg;", name);
            continue;
        }
        if (open_with(opened, *backend, codec, params)) {
            got = true;
            break;
        }
        errors += std::format(" {}: open failed;", name);
    }
    if (!got) {
        fail(std::format(
            "no {} encoder available:{}", h264 ? "H.264" : "HEVC", errors));
    }

    m_kind = opened.kind;
    m_codec = params.codec;
    m_enc = std::move(opened.enc);
    m_hw_device = std::move(opened.hw_device);
    m_hw_frames = std::move(opened.hw_frames);
    m_sw_pix = opened.sw_pix;
    m_hw_upload = opened.hw_upload;

    // libx265 and the hardware wrappers hand back packets with no duration. The
    // mp4 muxer derives each sample's duration from the next packet, so with
    // nothing following it the final sample is silently dropped -- one frame
    // short, and the file a frame shorter than the source. MPEG-TS does not
    // care, which is why this only ever showed up in mp4.
    m_frame_duration = av_rescale_q(1, av_inv_q(params.fps), m_enc.p->time_base);
    if (m_frame_duration <= 0) {
        m_frame_duration = 1;
    }
}

AVFrame* RoiEncoder::to_encoder_format(AVFrame* src)
{
    const bool same = src->format == m_sw_pix && src->width == m_enc.p->width
        && src->height == m_enc.p->height;
    if (same) {
        return src;
    }

    if (!m_sw_frame_ready) {
        m_sw_frame.p->format = m_sw_pix;
        m_sw_frame.p->width = m_enc.p->width;
        m_sw_frame.p->height = m_enc.p->height;
        check(av_frame_get_buffer(m_sw_frame.p, 32), "sw frame buffer");
        m_sw_frame_ready = true;
    }

    if (!m_sws.p || src->width != m_sws_src_w || src->height != m_sws_src_h
        || src->format != m_sws_src_fmt) {
        m_sws.reset();
        m_sws.p = sws_getContext(src->width, src->height,
            static_cast<AVPixelFormat>(src->format), m_enc.p->width,
            m_enc.p->height, m_sw_pix, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws.p) {
            fail("sws_getContext");
        }
        m_sws_src_w = src->width;
        m_sws_src_h = src->height;
        m_sws_src_fmt = src->format;
    }

    check(av_frame_make_writable(m_sw_frame.p), "make writable");
    sws_scale(m_sws.p, src->data, src->linesize, 0, src->height,
        m_sw_frame.p->data, m_sw_frame.p->linesize);
    return m_sw_frame.p;
}

void RoiEncoder::drain()
{
    for (;;) {
        const int r = avcodec_receive_packet(m_enc.p, m_pkt.p);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            break;
        }
        check(r, "receive packet");
        if (m_pkt.p->duration <= 0) {
            m_pkt.p->duration = m_frame_duration;
        }
        m_sink->on_packet(m_pkt.p, m_enc.p->time_base);
        av_packet_unref(m_pkt.p);
    }
}

void RoiEncoder::encode(
    AVFrame* frame, const std::vector<Rect>& rects, int64_t pts)
{
    AVFrame* sw = to_encoder_format(frame);
    sw->pts = pts;
    // A frame handed straight through still carries the decoder's picture type.
    // Choosing I/P/B is the encoder's job, and honouring the decoder's answer
    // would let the input's GOP structure override the requested one.
    sw->pict_type = AV_PICTURE_TYPE_NONE;
    attach_roi(sw, rects);

    if (m_hw_upload) {
        roiav::Frame hw;
        check(av_hwframe_get_buffer(m_enc.p->hw_frames_ctx, hw.p, 0),
            "hwframe get");
        check(av_hwframe_transfer_data(hw.p, sw, 0), "hw upload");
        hw.p->pts = pts;
        // Side data does not survive the transfer to hardware.
        attach_roi(hw.p, rects);
        check(avcodec_send_frame(m_enc.p, hw.p), "send frame");
    } else {
        check(avcodec_send_frame(m_enc.p, sw), "send frame");
    }
    drain();
}

void RoiEncoder::flush()
{
    check(avcodec_send_frame(m_enc.p, nullptr), "flush encoder");
    drain();
}

} // namespace smartvideo
