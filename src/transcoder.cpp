#include "transcoder.h"

#include "qp_delta_map.h"
#include "roi_map_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace {
struct AvFmtIn {
    AVFormatContext* p = nullptr;
    ~AvFmtIn()
    {
        if (p) {
            avformat_close_input(&p);
        }
    }
};

struct AvFmtOut {
    AVFormatContext* p = nullptr;
    ~AvFmtOut()
    {
        if (p) {
            if (p->oformat && !(p->oformat->flags & AVFMT_NOFILE) && p->pb) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};

struct AvCodecCtx {
    AVCodecContext* p = nullptr;
    AvCodecCtx() = default;
    AvCodecCtx(AvCodecCtx&& o) noexcept : p(o.p) { o.p = nullptr; }
    AvCodecCtx& operator=(AvCodecCtx&& o) noexcept
    {
        if (this != &o) {
            avcodec_free_context(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~AvCodecCtx() { avcodec_free_context(&p); }
};

struct AvFrame {
    AVFrame* p = nullptr;
    AvFrame() : p(av_frame_alloc()) {}
    ~AvFrame() { av_frame_free(&p); }
    AvFrame(const AvFrame&) = delete;
    AvFrame& operator=(const AvFrame&) = delete;
};

struct AvPacket {
    AVPacket* p = nullptr;
    AvPacket() : p(av_packet_alloc()) {}
    ~AvPacket() { av_packet_free(&p); }
};

struct AvSws {
    SwsContext* p = nullptr;
    ~AvSws()
    {
        if (p) {
            sws_freeContext(p);
        }
    }
};

struct AvBuffer {
    AVBufferRef* p = nullptr;
    AvBuffer() = default;
    AvBuffer(AvBuffer&& o) noexcept : p(o.p) { o.p = nullptr; }
    AvBuffer& operator=(AvBuffer&& o) noexcept
    {
        if (this != &o) {
            av_buffer_unref(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~AvBuffer() { av_buffer_unref(&p); }
};

[[noreturn]] void fail(const std::string& msg)
{
    throw std::runtime_error(msg);
}

void check(int err, const char* what)
{
    if (err >= 0) {
        return;
    }
    char buf[128];
    av_strerror(err, buf, sizeof(buf));
    fail(std::format("{}: {}", what, buf));
}

int find_stream(AVFormatContext* fmt, AVMediaType type)
{
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == type) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

AVRational guess_fps(AVStream* st, AVCodecContext* dec)
{
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        return st->avg_frame_rate;
    }
    if (dec->framerate.num > 0 && dec->framerate.den > 0) {
        return dec->framerate;
    }
    return AVRational{25, 1};
}

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

const char* encoder_label(EncoderKind k)
{
    switch (k) {
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

bool open_hw_device(AvBuffer& hw, AVHWDeviceType type, const char* device)
{
    av_buffer_unref(&hw.p);
    return av_hwdevice_ctx_create(&hw.p, type, device, nullptr, 0) >= 0;
}

bool init_vaapi_frames(AvBuffer& hw, AvBuffer& frames, int w, int h)
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

struct OpenedEncoder {
    EncoderKind kind = EncoderKind::X265;
    AvCodecCtx enc;
    AvBuffer hw_device;
    AvBuffer hw_frames;
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    bool hw_upload = false;
};

bool probe_one_frame(OpenedEncoder& enc)
{
    AvFrame f;
    f.p->format = enc.sw_pix;
    f.p->width = enc.enc.p->width;
    f.p->height = enc.enc.p->height;
    if (av_frame_get_buffer(f.p, 32) < 0) {
        return false;
    }
    if (av_frame_make_writable(f.p) < 0) {
        return false;
    }
    const int h = enc.enc.p->height;
    std::memset(f.p->data[0], 0, static_cast<size_t>(f.p->linesize[0]) * h);
    if (f.p->data[1]) {
        std::memset(f.p->data[1], 128,
            static_cast<size_t>(f.p->linesize[1]) * ((h + 1) / 2));
    }
    f.p->pts = 0;

    AVFrame* send = f.p;
    AvFrame hw;
    if (enc.hw_upload) {
        if (av_hwframe_get_buffer(enc.enc.p->hw_frames_ctx, hw.p, 0) < 0
            || av_hwframe_transfer_data(hw.p, f.p, 0) < 0) {
            return false;
        }
        hw.p->pts = 0;
        send = hw.p;
    }

    if (avcodec_send_frame(enc.enc.p, send) < 0) {
        return false;
    }
    AvPacket pkt;
    int r = avcodec_receive_packet(enc.enc.p, pkt.p);
    if (r == AVERROR(EAGAIN)) {
        if (avcodec_send_frame(enc.enc.p, nullptr) < 0) {
            return false;
        }
        r = avcodec_receive_packet(enc.enc.p, pkt.p);
    }
    return r >= 0;
}

bool try_open(
    OpenedEncoder& out,
    EncoderKind kind,
    const AVCodec* codec,
    AVCodecContext* dec,
    AVRational fps,
    int crf,
    int gop,
    const std::string& preset,
    bool probe)
{
    out = OpenedEncoder{};
    out.kind = kind;
    out.enc.p = avcodec_alloc_context3(codec);
    if (!out.enc.p) {
        return false;
    }
    out.enc.p->width = dec->width;
    out.enc.p->height = dec->height;
    out.enc.p->time_base = av_inv_q(fps);
    out.enc.p->framerate = fps;
    const int gop_auto = std::max(fps.num / std::max(fps.den, 1), 1) * 2;
    out.enc.p->gop_size = gop > 0 ? gop : gop_auto;
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
        av_opt_set(out.enc.p->priv_data, "preset", preset.c_str(), 0);
        out.enc.p->flags |= AV_CODEC_FLAG_QSCALE;
        out.enc.p->global_quality = crf * FF_QP2LAMBDA;
    } else if (kind == EncoderKind::Vaapi) {
        if (!open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128")
            && !open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr)) {
            return false;
        }
        if (!init_vaapi_frames(
                out.hw_device, out.hw_frames, dec->width, dec->height)) {
            return false;
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_VAAPI;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;
        av_opt_set_int(out.enc.p->priv_data, "qp", crf, 0);
        av_opt_set(out.enc.p->priv_data, "rc_mode", "CQP", 0);
    } else if (kind == EncoderKind::Nvenc) {
        open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_CUDA, nullptr);
        if (out.hw_device.p) {
            out.enc.p->hw_device_ctx = av_buffer_ref(out.hw_device.p);
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_NV12;
        out.sw_pix = AV_PIX_FMT_NV12;
        av_opt_set(out.enc.p->priv_data, "preset", preset.c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "rc", "constqp", 0);
        av_opt_set_int(out.enc.p->priv_data, "qp", crf, 0);
        av_opt_set(out.enc.p->priv_data, "spatial-aq", "0", 0);
        av_opt_set(out.enc.p->priv_data, "temporal-aq", "0", 0);
    } else {
        out.enc.p->pix_fmt = AV_PIX_FMT_YUV420P;
        out.sw_pix = AV_PIX_FMT_YUV420P;
        av_opt_set(out.enc.p->priv_data, "preset", preset.c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "crf", std::to_string(crf).c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "aq-mode", "1", 0);
    }

    if (avcodec_open2(out.enc.p, codec, nullptr) < 0) {
        out = OpenedEncoder{};
        return false;
    }
    if (kind == EncoderKind::Qsv && probe) {
        const bool ok = probe_one_frame(out);
        out = OpenedEncoder{};
        if (!ok) {
            return false;
        }
        return try_open(out, kind, codec, dec, fps, crf, gop, preset, false);
    }
    return true;
}

OpenedEncoder open_encoder(
    EncoderKind want,
    AVCodecContext* dec,
    AVRational fps,
    int crf,
    int gop,
    const std::string& preset)
{
    struct Candidate {
        EncoderKind kind;
        const char* name;
    };
    std::vector<Candidate> order;
    if (want == EncoderKind::Auto) {
        order = {
            {EncoderKind::Qsv, "hevc_qsv"},
            {EncoderKind::Vaapi, "hevc_vaapi"},
            {EncoderKind::X265, "libx265"},
        };
    } else if (want == EncoderKind::Qsv) {
        order = {{EncoderKind::Qsv, "hevc_qsv"}};
    } else if (want == EncoderKind::Vaapi) {
        order = {{EncoderKind::Vaapi, "hevc_vaapi"}};
    } else if (want == EncoderKind::Nvenc) {
        order = {{EncoderKind::Nvenc, "hevc_nvenc"}};
    } else {
        order = {{EncoderKind::X265, "libx265"}};
    }

    OpenedEncoder opened;
    std::string errors;
    for (const auto& c : order) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (!codec && c.kind == EncoderKind::X265) {
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        }
        if (!codec) {
            errors += std::format(" {}: not in FFmpeg;", c.name);
            continue;
        }
        if (try_open(opened, c.kind, codec, dec, fps, crf, gop, preset, true)) {
            return opened;
        }
        errors += std::format(" {}: open failed;", c.name);
    }
    fail("no HEVC encoder available:" + errors);
}
} // namespace

void Transcoder::run(const TranscodeOptions& opt, std::atomic_bool* cancel)
{
    AvFmtIn in;
    check(avformat_open_input(&in.p, opt.input_path.c_str(), nullptr, nullptr),
        "open input");
    check(avformat_find_stream_info(in.p, nullptr), "find stream info");

    const int vindex = find_stream(in.p, AVMEDIA_TYPE_VIDEO);
    if (vindex < 0) {
        fail("no video stream");
    }
    const int aindex = find_stream(in.p, AVMEDIA_TYPE_AUDIO);
    AVStream* in_v = in.p->streams[vindex];

    const AVCodec* decoder = avcodec_find_decoder(in_v->codecpar->codec_id);
    if (!decoder) {
        fail("no decoder for input video");
    }
    AvCodecCtx dec;
    dec.p = avcodec_alloc_context3(decoder);
    if (!dec.p) {
        fail("alloc decoder");
    }
    check(avcodec_parameters_to_context(dec.p, in_v->codecpar),
        "decoder params");
    check(avcodec_open2(dec.p, decoder, nullptr), "open decoder");

    const AVRational fps = guess_fps(in_v, dec.p);
    OpenedEncoder enc =
        open_encoder(opt.encoder, dec.p, fps, opt.crf, opt.gop, opt.preset);

    AvFmtOut out;
    check(avformat_alloc_output_context2(
              &out.p, nullptr, nullptr, opt.output_path.c_str()),
        "alloc output");
    AVStream* out_v = avformat_new_stream(out.p, nullptr);
    if (!out_v) {
        fail("new video stream");
    }
    check(avcodec_parameters_from_context(out_v->codecpar, enc.enc.p),
        "encoder params");
    out_v->time_base = enc.enc.p->time_base;

    AVStream* out_a = nullptr;
    if (aindex >= 0) {
        out_a = avformat_new_stream(out.p, nullptr);
        if (!out_a) {
            fail("new audio stream");
        }
        check(avcodec_parameters_copy(
                  out_a->codecpar, in.p->streams[aindex]->codecpar),
            "copy audio params");
        out_a->time_base = in.p->streams[aindex]->time_base;
    }

    if (!(out.p->oformat->flags & AVFMT_NOFILE)) {
        check(avio_open(&out.p->pb, opt.output_path.c_str(), AVIO_FLAG_WRITE),
            "avio_open");
    }
    check(avformat_write_header(out.p, nullptr), "write header");

    AvSws sws;
    sws.p = sws_getContext(dec.p->width, dec.p->height, dec.p->pix_fmt,
        enc.enc.p->width, enc.enc.p->height, enc.sw_pix, SWS_BILINEAR, nullptr,
        nullptr, nullptr);
    if (!sws.p) {
        fail("sws_getContext");
    }

    AvFrame dec_frame;
    AvFrame sw_frame;
    sw_frame.p->format = enc.sw_pix;
    sw_frame.p->width = enc.enc.p->width;
    sw_frame.p->height = enc.enc.p->height;
    check(av_frame_get_buffer(sw_frame.p, 32), "sw frame buffer");

    RoiMapBuilder roi_builder;
    QpDeltaMapBuilder qp_map;
    AvPacket ipkt;
    AvPacket opkt;
    bool logged_roi = false;

    auto encode_frame = [&](AVFrame* frame) {
        check(avcodec_send_frame(enc.enc.p, frame), "send frame");
        for (;;) {
            const int r = avcodec_receive_packet(enc.enc.p, opkt.p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                break;
            }
            check(r, "receive packet");
            av_packet_rescale_ts(
                opkt.p, enc.enc.p->time_base, out_v->time_base);
            opkt.p->stream_index = out_v->index;
            check(av_interleaved_write_frame(out.p, opkt.p), "write video");
            av_packet_unref(opkt.p);
        }
    };

    auto handle_decoded = [&]() {
        const int64_t pts = dec_frame.p->best_effort_timestamp;
        const double t = (pts == AV_NOPTS_VALUE)
            ? 0.0
            : static_cast<double>(pts) * av_q2d(in_v->time_base);

        check(av_frame_make_writable(sw_frame.p), "make writable");
        sws_scale(sws.p, dec_frame.p->data, dec_frame.p->linesize, 0,
            dec.p->height, sw_frame.p->data, sw_frame.p->linesize);
        sw_frame.p->pts = av_rescale_q(pts == AV_NOPTS_VALUE ? 0 : pts,
            in_v->time_base, enc.enc.p->time_base);

        const auto dets = roi_builder.detections_at(
            opt.roi, enc.enc.p->width, enc.enc.p->height, t);
        const auto rects = roi_builder.build(enc.enc.p->width,
            enc.enc.p->height, dets, opt.roi.background_qp_delta);
        attach_roi(sw_frame.p, rects);
        if (!logged_roi) {
            const auto grid = qp_map.build(
                enc.enc.p->width, enc.enc.p->height, 32, rects);
            std::fprintf(stderr,
                "ROI: %zu rects, HEVC qpDeltaMap %dx%d CTU (32x32)\n",
                rects.size(), grid.cols, grid.rows);
            if (enc.kind == EncoderKind::Nvenc) {
                std::fprintf(stderr,
                    "warning: stock FFmpeg hevc_nvenc ignores ROI; "
                    "qpDeltaMap is built but not submitted to NVENC SDK\n");
            }
            logged_roi = true;
        }

        if (enc.hw_upload) {
            AvFrame hw_frame;
            check(av_hwframe_get_buffer(enc.enc.p->hw_frames_ctx, hw_frame.p, 0),
                "hwframe get");
            check(av_hwframe_transfer_data(hw_frame.p, sw_frame.p, 0),
                "hw upload");
            hw_frame.p->pts = sw_frame.p->pts;
            attach_roi(hw_frame.p, rects);
            encode_frame(hw_frame.p);
        } else {
            encode_frame(sw_frame.p);
        }
    };

    while (av_read_frame(in.p, ipkt.p) >= 0) {
        if (cancel && cancel->load()) {
            fail("cancelled");
        }
        if (ipkt.p->stream_index == vindex) {
            check(avcodec_send_packet(dec.p, ipkt.p), "send packet");
            av_packet_unref(ipkt.p);
            for (;;) {
                const int r = avcodec_receive_frame(dec.p, dec_frame.p);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                    break;
                }
                check(r, "receive frame");
                handle_decoded();
                av_frame_unref(dec_frame.p);
            }
            continue;
        }
        if (out_a && ipkt.p->stream_index == aindex) {
            ipkt.p->stream_index = out_a->index;
            av_packet_rescale_ts(ipkt.p, in.p->streams[aindex]->time_base,
                out_a->time_base);
            check(av_interleaved_write_frame(out.p, ipkt.p), "write audio");
        }
        av_packet_unref(ipkt.p);
    }

    check(avcodec_send_packet(dec.p, nullptr), "flush decoder send");
    for (;;) {
        const int r = avcodec_receive_frame(dec.p, dec_frame.p);
        if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
            break;
        }
        check(r, "flush decoder");
        handle_decoded();
        av_frame_unref(dec_frame.p);
    }

    encode_frame(nullptr);
    check(av_write_trailer(out.p), "write trailer");
    std::fprintf(stderr, "encoded with %s crf=%d preset=%s\n",
        encoder_label(enc.kind), opt.crf, opt.preset.c_str());
}
