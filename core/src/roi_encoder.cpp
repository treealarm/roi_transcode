#include "roi_encoder.h"

#include "packet_sink.h"

#include <algorithm>
#include <cstring>
#include <format>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

using roiav::check;
using roiav::fail;

namespace {

void attach_roi(AVFrame* frame, const std::vector<RoiRect>& rects)
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
    switch (m_kind) {
    case EncoderKind::Qsv:
        return "hevc_qsv (Intel VPL mfxExtEncoderROI)";
    case EncoderKind::Vaapi:
        return "hevc_vaapi (Intel iHD VAEncROI)";
    case EncoderKind::Nvenc:
        return "hevc_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)";
    default:
        return "libx265 (CPU fallback)";
    }
}

namespace {

// One attempt at one backend. `probe` asks for a throwaway encode of a single
// frame before the context is handed back: Ubuntu's older iHD opens hevc_qsv
// happily and then returns empty packets, and only an actual encode tells the
// difference.
struct Attempt {
    EncoderKind kind = EncoderKind::X265;
    roiav::CodecCtx enc;
    roiav::Buffer hw_device;
    roiav::Buffer hw_frames;
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    bool hw_upload = false;
};

bool probe_one_frame(Attempt& a)
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

bool try_open(Attempt& out, EncoderKind kind, const AVCodec* codec,
    const EncoderParams& p, bool probe)
{
    out = Attempt{};
    out.kind = kind;
    out.enc.p = avcodec_alloc_context3(codec);
    if (!out.enc.p) {
        return false;
    }
    out.enc.p->width = p.width;
    out.enc.p->height = p.height;
    out.enc.p->time_base =
        p.time_base.den > 0 ? p.time_base : av_inv_q(p.fps);
    out.enc.p->framerate = p.fps;
    const int gop_auto = std::max(p.fps.num / std::max(p.fps.den, 1), 1) * 2;
    out.enc.p->gop_size = p.gop > 0 ? p.gop : gop_auto;
    out.enc.p->max_b_frames = 0;

    if (kind == EncoderKind::Qsv) {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "child_device_type", "vaapi", 0);
        const int derr = av_hwdevice_ctx_create(
            &out.hw_device.p, AV_HWDEVICE_TYPE_QSV, "auto", opts, 0);
        av_dict_free(&opts);
        if (derr < 0
            && !open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_QSV, nullptr)) {
            return false;
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_NV12;
        out.enc.p->hw_device_ctx = av_buffer_ref(out.hw_device.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        // Alder Lake HEVC is EncSliceLP only: VDENC + CQP. ICQ/global_quality fails.
        // CQP is also the mode where mfxExtEncoderROI DeltaQP applies.
        av_opt_set(out.enc.p->priv_data, "low_power", "1", 0);
        av_opt_set(out.enc.p->priv_data, "gpb", "0", 0);
        av_opt_set(out.enc.p->priv_data, "preset", p.preset.c_str(), 0);
        out.enc.p->flags |= AV_CODEC_FLAG_QSCALE;
        out.enc.p->global_quality = p.crf * FF_QP2LAMBDA;
    } else if (kind == EncoderKind::Vaapi) {
        if (!open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128")
            && !open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr)) {
            return false;
        }
        if (!init_vaapi_frames(
                out.hw_device, out.hw_frames, p.width, p.height)) {
            return false;
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_VAAPI;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        av_opt_set(out.enc.p->priv_data, "rc_mode", "CQP", 0);
    } else if (kind == EncoderKind::Nvenc) {
        open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_CUDA, nullptr);
        if (out.hw_device.p) {
            out.enc.p->hw_device_ctx = av_buffer_ref(out.hw_device.p);
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_NV12;
        out.sw_pix = AV_PIX_FMT_NV12;
        av_opt_set(out.enc.p->priv_data, "preset", p.preset.c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "rc", "constqp", 0);
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        av_opt_set(out.enc.p->priv_data, "spatial-aq", "0", 0);
        av_opt_set(out.enc.p->priv_data, "temporal-aq", "0", 0);
    } else {
        out.enc.p->pix_fmt = AV_PIX_FMT_YUV420P;
        out.sw_pix = AV_PIX_FMT_YUV420P;
        av_opt_set(out.enc.p->priv_data, "preset", p.preset.c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "crf", std::to_string(p.crf).c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "aq-mode", "1", 0);
    }

    if (avcodec_open2(out.enc.p, codec, nullptr) < 0) {
        out = Attempt{};
        return false;
    }
    if (kind == EncoderKind::Qsv && probe) {
        const bool ok = probe_one_frame(out);
        out = Attempt{};
        if (!ok) {
            return false;
        }
        return try_open(out, kind, codec, p, false);
    }
    return true;
}

} // namespace

void RoiEncoder::open(const EncoderParams& params, PacketSink& sink)
{
    struct Candidate {
        EncoderKind kind;
        const char* name;
    };
    std::vector<Candidate> order;
    switch (params.kind) {
    case EncoderKind::Auto:
        order = {
            {EncoderKind::Qsv, "hevc_qsv"},
            {EncoderKind::Vaapi, "hevc_vaapi"},
            {EncoderKind::X265, "libx265"},
        };
        break;
    case EncoderKind::Qsv:
        order = {{EncoderKind::Qsv, "hevc_qsv"}};
        break;
    case EncoderKind::Vaapi:
        order = {{EncoderKind::Vaapi, "hevc_vaapi"}};
        break;
    case EncoderKind::Nvenc:
        order = {{EncoderKind::Nvenc, "hevc_nvenc"}};
        break;
    default:
        order = {{EncoderKind::X265, "libx265"}};
        break;
    }

    Attempt opened;
    std::string errors;
    bool got = false;
    for (const auto& c : order) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (!codec && c.kind == EncoderKind::X265) {
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        }
        if (!codec) {
            errors += std::format(" {}: not in FFmpeg;", c.name);
            continue;
        }
        if (try_open(opened, c.kind, codec, params, true)) {
            got = true;
            break;
        }
        errors += std::format(" {}: open failed;", c.name);
    }
    if (!got) {
        fail("no HEVC encoder available:" + errors);
    }

    m_kind = opened.kind;
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

    m_sink = &sink;
    m_sink->begin(m_enc.p);
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
    AVFrame* frame, const std::vector<RoiRect>& rects, int64_t pts)
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
