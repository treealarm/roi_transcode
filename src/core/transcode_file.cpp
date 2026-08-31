#include "sve/sve.h"

#include "av_raii.h"
#include "codec_id.h"
#include "packet_sink.h"
#include "temporal_roi.h"
#include "sve/encoder.h"
#include "sve/frame.h"

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
    return codec_from_av_id(input);
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

    EncoderConfig cfg;
    cfg.input.width = dec.p->width;
    cfg.input.height = dec.p->height;
    cfg.input.frame_rate = Rational{fps.num, fps.den};
    // One tick per frame, said rather than left to a default. InputVideoFormat's own
    // default is §8.3's 1/90000, which is right for a pushed source carrying its own clock
    // and wrong here: a file has no clock but its frame rate, and encoding it against
    // 90 kHz moves every PTS and with it the rate control's answers.
    cfg.input.time_base = Rational{fps.den, fps.num};
    cfg.settings = settings;
    cfg.backend_selection = cfg.settings.backend_selection;
    // Resolved here rather than inside the encoder: only this entry point has a source to
    // follow, and an encoder handed frames has nothing to answer SameAsSource from.
    cfg.settings.codec = resolve_codec(settings.codec, in_v->codecpar->codec_id);

    // Left to propagate: EncoderError is a std::runtime_error, and this entry point
    // already answers a whole file with one exception. Re-wrapping it in fail() would
    // throw away the StatusCode a caller can branch on.
    const std::unique_ptr<SmartVideoEncoder> made = CreateVideoEncoder(cfg);
    SmartVideoEncoder& enc = *made;

    // §13. Built once from the whole timeline: grouping boxes into tracks per frame would be
    // the same work repeated for every frame of the clip.
    const TemporalRoi temporal(boxes, settings.temporal, settings.quality.hold_sec);
    roiav::Frame dec_frame;
    roiav::Packet ipkt;
    bool mux_open = false;

    CodecParameters params;
    if (const Status s = enc.GetCodecParameters(&params); !s.ok()) {
        fail(s.message());
    }

    // The muxer needs the encoder's stream parameters, and those exist as soon as a backend
    // has opened -- which Create has already done. So the stream is declared before the
    // first frame rather than from inside the packet path, and nothing here has to hold an
    // AVCodecContext to do it.
    auto open_mux = [&]() {
        if (!mux_open) {
            file_sink.begin(params);
            mux_open = true;
        }
    };

    auto drain = [&]() {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc.ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return;
            }
            if (!s.ok()) {
                fail(s.message());
            }
            open_mux();
            file_sink.on_packet(packet);
        }
    };

    auto handle_decoded = [&]() {
        const int64_t pts = dec_frame.p->best_effort_timestamp;
        const double t = (pts == AV_NOPTS_VALUE)
            ? 0.0
            : static_cast<double>(pts) * av_q2d(in_v->time_base);

        // One region per track rather than one per sample, positioned where the object was
        // at this instant rather than where it was last seen.
        const std::vector<Box> live = temporal.at(t);

        FrameMetadata meta;
        const AVRational enc_tb{params.time_base.num, params.time_base.den};
        meta.timestamp = MediaTimestamp{
            av_rescale_q(pts == AV_NOPTS_VALUE ? 0 : pts, in_v->time_base, enc_tb),
            params.time_base};
        meta.regions.boxes = live;

        const Status s = SubmitAvFrame(enc, dec_frame.p, meta);
        if (!s.ok()) {
            fail(s.message());
        }
        drain();
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
            open_mux();
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

    const Status flushed = enc.Flush();
    if (!flushed.ok()) {
        fail(flushed.message());
    }
    drain();
    open_mux();
    file_sink.end();

    const Report report = enc.ToReport();
    std::fprintf(stderr, "encoded with %s crf=%d preset=%s converted=%ld uploaded=%ld\n",
        report.backend_name.c_str(), settings.crf, preset_name(settings.preset),
        static_cast<long>(report.frames_converted),
        static_cast<long>(report.frames_uploaded));
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
