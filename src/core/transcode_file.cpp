#include "sve/sve.h"

#include "av_raii.h"
#include "bitrate_budget.h"
#include "box_smoother.h"
#include "encoder_runtime_warnings.h"
#include "frame_roi_pipeline.h"
#include "packet_sink.h"
#include "importance_map.h"
#include "roi_lag.h"
#include "roi_encoder.h"
#include "temporal_roi.h"
#include "object_normalizer.h"
#include "qp_slew.h"
#include "sve/metadata.h"

#include <cstdio>
#include <vector>

namespace sve {

namespace {

/// Codec::SameAsSource, answered from the input. Anything that is neither H.264 nor HEVC becomes
/// H.264: it is the codec every player handles, and guessing a niche one back at the caller would
/// be worse than the one answer that always works.
Codec resolve_codec(Codec asked, AVCodecID input)
{
    if (asked != Codec::SameAsSource) {
        return asked;
    }
    return input == AV_CODEC_ID_HEVC ? Codec::H265 : Codec::H264;
}

} // namespace


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

Report transcode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Settings& settings, std::atomic_bool* cancel)
{
    roiav::FmtIn in;
    check(avformat_open_input(&in.p, input_path.c_str(), nullptr, nullptr),
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

    const AVRational fps = guess_fps(in_v, dec.p);

    FileMuxSink file_sink(output_path, settings.movflags);
    if (aindex >= 0) {
        file_sink.declare_audio(
            in.p->streams[aindex]->codecpar, in.p->streams[aindex]->time_base);
    }

    BitrateBudget bitrate;
    QpSlewLimiter qp_slew;
    BoxSmoother box_smooth;
    const double frame_sec = (fps.num > 0 && fps.den > 0)
        ? static_cast<double>(fps.den) / static_cast<double>(fps.num)
        : 1.0 / 25.0;

    struct BudgetedMuxSink : PacketSink {
        FileMuxSink& inner;
        BitrateBudget& budget;
        double fallback_sec;
        BudgetedMuxSink(FileMuxSink& i, BitrateBudget& b, double fs)
            : inner(i), budget(b), fallback_sec(fs)
        {
        }
        void begin(AVCodecContext* enc) override { inner.begin(enc); }
        void on_packet(AVPacket* pkt, AVRational tb) override
        {
            if (budget.active()) {
                double dt = 0.0;
                if (pkt->duration > 0 && tb.den > 0) {
                    dt = static_cast<double>(pkt->duration) * tb.num / tb.den;
                }
                budget.observe(pkt->size, dt > 0.0 ? dt : fallback_sec);
            }
            inner.on_packet(pkt, tb);
        }
        void end() override { inner.end(); }
    } sink(file_sink, bitrate, frame_sec);

    EncoderConfig ep;
    ep.width = dec.p->width;
    ep.height = dec.p->height;
    ep.fps = Rational{fps.num, fps.den};
    ep.kind = settings.encoder;
    ep.policy = settings.policy;
    ep.temporal = settings.temporal;
    ep.rate_control = settings.rate_control;
    ep.codec = resolve_codec(settings.codec, in_v->codecpar->codec_id);
    bool cpu_h265_target_guard = false;
    bool logged_sei_unsupported = false;
    if (ep.kind == Encoder::Cpu && ep.codec == Codec::H265) {
        // Guard: CPU HEVC target-bitrate path is unstable on our stack.
        cpu_h265_target_guard = ep.rate_control.target_bitrate > 0;
        ep.rate_control.target_bitrate = 0;
        if (cpu_h265_target_guard) {
            std::fprintf(stderr,
                "guard: cpu h265 target-bitrate disabled; using CRF/CQP\n");
        }
    }
    ep.crf = settings.crf;
    ep.gop = settings.gop;
    ep.preset = settings.preset;
    // Keep file mode aligned with Session: apply sanitised rate-control (after guards).
    bitrate = BitrateBudget(ep.rate_control);

    RoiEncoder enc;
    enc.open(ep, sink);
    RoiMapBuilder roi_builder;
    // §13. Built once from the whole timeline: grouping boxes into tracks per frame would be
    // the same work repeated for every frame of the clip.
    const TemporalRoi temporal(boxes, settings.temporal, settings.quality.hold_sec);
    QpDeltaMapBuilder qp_map;
    roiav::Frame dec_frame;
    roiav::Packet ipkt;
    bool logged_roi = false;
    int64_t frames = 0;
    int64_t dropped = 0;
    std::vector<RoiLagSample> lag_samples;

    auto handle_decoded = [&]() {
        const int64_t pts = dec_frame.p->best_effort_timestamp;
        const double t = (pts == AV_NOPTS_VALUE)
            ? 0.0
            : static_cast<double>(pts) * av_q2d(in_v->time_base);

        // One region per track rather than one per sample, positioned where the object was
        // at this instant rather than where it was last seen.
        const EncoderCapabilities caps = enc.capabilities();
        const std::vector<Box> live = temporal.at(t);
        FrameRoiPipelineInput pipeline_in{
            .boxes = live,
            .quality = settings.quality,
            .temporal = settings.temporal,
            .caps = caps,
            .width = ep.width,
            .height = ep.height,
            .frame_interval_sec = frame_sec,
        };
        FrameRoiPipeline pipeline(bitrate, box_smooth, qp_slew, roi_builder);
        FrameRoiPipelineOutput roi_out = pipeline.process(pipeline_in);
        auto& rects = roi_out.rects;
        dropped += roi_out.regions_dropped;
        lag_samples.insert(lag_samples.end(), roi_out.lag_samples.begin(), roi_out.lag_samples.end());

        if (!logged_roi) {
            const int32_t block = roi_out.coding_block;
            const auto grid = qp_map.build(ep.width, ep.height, block, rects);
            std::fprintf(stderr, "ROI: %zu rects, qpDeltaMap %dx%d blocks of %d%s\n",
                rects.size(), grid.cols, grid.rows, block,
                caps.qp_map ? " (qp_map)" : "");
            if (enc.kind() == Encoder::Nvenc) {
                std::fprintf(stderr,
                    "warning: stock FFmpeg hevc_nvenc ignores ROI; "
                    "qpDeltaMap is built but not submitted to NVENC SDK\n");
            }
            logged_roi = true;
        }

        const int64_t out_pts = av_rescale_q(
            pts == AV_NOPTS_VALUE ? 0 : pts, in_v->time_base, enc.time_base());
        std::vector<uint8_t> sei;
        warn_sei_unsupported_once(settings.metadata.bitstream_sei, caps, logged_sei_unsupported);
        if (settings.metadata.bitstream_sei && caps.sei) {
            SeiFrame meta;
            meta.frame_id = static_cast<uint64_t>(frames);
            meta.pts = out_pts;
            meta.objects =
                sei_objects_from_boxes(roi_out.applied_boxes, settings.metadata.max_objects);
            sei = encode_sei_side_data(meta, settings.metadata.max_objects);
        }
        enc.encode(dec_frame.p, rects, out_pts, sei);
        ++frames;
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
        if (file_sink.has_audio() && ipkt.p->stream_index == aindex) {
            file_sink.write_audio(ipkt.p, in.p->streams[aindex]->time_base);
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
    Report report;
    report.encoder_used = enc.kind();
    report.encoder_name = enc.label();
    report.frames = frames;
    report.regions_dropped = dropped;
    report.frames_converted = enc.frames_converted();
    report.frames_uploaded = enc.frames_uploaded();
    report.cpu_h265_target_guard = cpu_h265_target_guard;
    fill_lag_report(lag_samples, report);
    std::fprintf(stderr, "encoded with %s crf=%d preset=%s converted=%ld uploaded=%ld\n",
        enc.label(), settings.crf, preset_name(settings.preset), enc.frames_converted(),
        enc.frames_uploaded());
    if (report.roi_lag_measured) {
        std::fprintf(stderr,
            "roi lag: samples=%lld min_iou=%.3f p95_frames=%.2f p99_frames=%.2f "
            "p95_ms=%.1f p99_ms=%.1f acceptance=%s\n",
            static_cast<long long>(report.roi_lag_samples), report.roi_lag_min_iou,
            report.roi_lag_p95_frames, report.roi_lag_p99_frames, report.roi_lag_p95_ms,
            report.roi_lag_p99_ms, report.roi_lag_acceptance ? "pass" : "fail");
    } else {
        std::fprintf(stderr, "roi lag: not measured (no tracked boxes)\n");
    }
    return report;
}

} // namespace sve
