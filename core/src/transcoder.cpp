#include "roi/transcoder.h"

#include "av_raii.h"
#include "packet_sink.h"
#include "qp_delta_map.h"
#include "roi_encoder.h"
#include "roi_map_builder.h"

#include <cstdio>
#include <vector>

using roiav::check;
using roiav::fail;

namespace {

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

} // namespace

void Transcoder::run(const TranscodeOptions& opt, std::atomic_bool* cancel)
{
    roiav::FmtIn in;
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
    roiav::CodecCtx dec;
    dec.p = avcodec_alloc_context3(decoder);
    if (!dec.p) {
        fail("alloc decoder");
    }
    check(avcodec_parameters_to_context(dec.p, in_v->codecpar), "decoder params");
    check(avcodec_open2(dec.p, decoder, nullptr), "open decoder");

    FileMuxSink sink(opt.output_path, opt.movflags);
    if (aindex >= 0) {
        sink.declare_audio(
            in.p->streams[aindex]->codecpar, in.p->streams[aindex]->time_base);
    }

    EncoderParams ep;
    ep.width = dec.p->width;
    ep.height = dec.p->height;
    ep.fps = guess_fps(in_v, dec.p);
    ep.kind = opt.encoder;
    ep.crf = opt.crf;
    ep.gop = opt.gop;
    ep.preset = opt.preset;

    RoiEncoder enc;
    enc.open(ep, sink);

    RoiMapBuilder roi_builder;
    QpDeltaMapBuilder qp_map;
    roiav::Frame dec_frame;
    roiav::Packet ipkt;
    bool logged_roi = false;

    auto handle_decoded = [&]() {
        const int64_t pts = dec_frame.p->best_effort_timestamp;
        const double t = (pts == AV_NOPTS_VALUE)
            ? 0.0
            : static_cast<double>(pts) * av_q2d(in_v->time_base);

        const auto dets = roi_builder.detections_at(
            opt.roi, ep.width, ep.height, t);
        const auto rects =
            roi_builder.build(ep.width, ep.height, dets, opt.roi.background_qp_delta);

        if (!logged_roi) {
            const auto grid = qp_map.build(ep.width, ep.height, 32, rects);
            std::fprintf(stderr,
                "ROI: %zu rects, HEVC qpDeltaMap %dx%d CTU (32x32)\n",
                rects.size(), grid.cols, grid.rows);
            if (enc.kind() == EncoderKind::Nvenc) {
                std::fprintf(stderr,
                    "warning: stock FFmpeg hevc_nvenc ignores ROI; "
                    "qpDeltaMap is built but not submitted to NVENC SDK\n");
            }
            logged_roi = true;
        }

        const int64_t out_pts = av_rescale_q(
            pts == AV_NOPTS_VALUE ? 0 : pts, in_v->time_base, enc.time_base());
        enc.encode(dec_frame.p, rects, out_pts);
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
        if (sink.has_audio() && ipkt.p->stream_index == aindex) {
            sink.write_audio(ipkt.p, in.p->streams[aindex]->time_base);
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

    enc.flush();
    sink.end();
    std::fprintf(stderr, "encoded with %s crf=%d preset=%s\n", enc.label(),
        opt.crf, opt.preset.c_str());
}
