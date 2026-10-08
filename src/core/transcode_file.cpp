#include "ta_roienc/ta_roienc.h"

#include "av_raii.h"
#include "backend/common/av_pixel_format.h"
#include "codec_id.h"
#include "file_mux_sink.h"
#include "temporal_roi.h"
#include "ta_roienc/encoder.h"
#include "ta_roienc/frame.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ta_roienc {

namespace {

/// What to tell the encoder its input is.
///
/// Without it a ten-bit source would be encoded as if it were eight: a Main10 clip through
/// hevc_qsv, which reports main10, still produced an 8-bit Main file with every frame
/// counted as converted.
///
/// The decoder's own format when the library has a name for it. Otherwise the depth is what
/// matters: ten bits become P010, because what the encoder needs here is the depth and the
/// profile that follows from it -- the pixels arrive as an AVFrame and the backend converts
/// the layout if it must, counting the conversion.
PixelFormat input_format_of(const AVCodecContext& dec)
{
    const PixelFormat named = from_av_pix_fmt(dec.pix_fmt);
    if (named != PixelFormat::Unknown) {
        return named;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dec.pix_fmt);
    if (desc != nullptr && desc->nb_components > 0 && desc->comp[0].depth > 8) {
        return PixelFormat::P010;
    }
    return PixelFormat::YUV420P;
}

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

    // Software, deliberately, where Session now decodes on the encoder's device.
    //
    // Two reasons, and neither is that it was forgotten. This is the file pass: a clip
    // re-encoded offline, bounded by the encoder rather than by wall-clock, so the CPU a
    // hardware decode would save is not on anyone's critical path -- unlike live video,
    // where it was most of what the transcode cost. And the ordering here is not free: the
    // encoder's frame rate is guessed from this decoder once it is open (guess_fps below),
    // so handing it the encoder's device means opening them the other way round and finding
    // another answer for the frame rate. Worth doing on its own, not as a rider here.
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
    // The picture is stored in one orientation and displayed in another; the decoder gives
    // us the stored one, so the matrix has to travel with it or the transcode comes out
    // rotated against its source.
    if (const AVPacketSideData* rotation = av_packet_side_data_get(
            in_v->codecpar->coded_side_data, in_v->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX)) {
        file_sink.declare_display_matrix(rotation->data, rotation->size);
    }

    EncoderConfig cfg;
    cfg.input.width = dec.p->width;
    cfg.input.height = dec.p->height;
    cfg.input.pixel_format = input_format_of(*dec.p);
    cfg.input.frame_rate = Rational{fps.num, fps.den};
    // One tick per frame, said rather than left to a default. InputVideoFormat's own
    // default is 1/90000, which is right for a pushed source carrying its own clock and
    // wrong here: a file has no clock but its frame rate, and encoding it against 90 kHz
    // moves every PTS and with it the rate control's answers.
    cfg.input.time_base = Rational{fps.den, fps.num};
    cfg.settings = settings;
    // Resolved here rather than inside the encoder: only this entry point has a source to
    // follow, and an encoder handed frames has nothing to answer SameAsSource from.
    cfg.settings.codec = resolve_codec(settings.codec, in_v->codecpar->codec_id);

    // Left to propagate: EncoderError is a std::runtime_error, and this entry point
    // already answers a whole file with one exception. Re-wrapping it in fail() would
    // throw away the StatusCode a caller can branch on.
    //
    // The library writes sidecar records, not files; naming the file is the application's,
    // which is this function. Opened only when a sidecar was asked for, and a sidecar that
    // cannot be opened is a failure rather than a silently missing file -- the caller asked
    // for metadata and would otherwise get an encode that looks complete without it.
    std::ofstream sidecar;
    std::string sidecar_path;
    if (cfg.settings.metadata.sidecar != SidecarFormat::None) {
        const std::filesystem::path base = std::filesystem::path(output_path);
        const std::filesystem::path path =
            base.parent_path() / (base.stem().string() + ".roimeta.ndjson");
        sidecar.open(path);
        if (!sidecar) {
            fail("cannot open sidecar " + path.string());
        }
        cfg.settings.metadata.sidecar_sink = [&sidecar](std::string_view record) {
            sidecar << record << '\n';
        };
        sidecar_path = path.string();
    }

    const std::unique_ptr<VideoEncoder> made = CreateVideoEncoder(cfg);
    VideoEncoder& enc = *made;

    // Built once from the whole timeline: grouping boxes into tracks per frame would be
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

    // The ten-bit path wants P010 and a decoder hands over whatever the file had, usually
    // planar yuv420p10le. Converting here rather than inside a backend is deliberate: the
    // native oneVPL component keeps FFmpeg out on purpose, and this is the example
    // application, which already has libswscale open for its own use.
    roiav::Sws to_p010;
    roiav::Frame p010_frame;
    auto as_encoder_wants = [&](AVFrame* decoded) -> AVFrame* {
        if (cfg.input.pixel_format != PixelFormat::P010
            || decoded->format == AV_PIX_FMT_P010) {
            return decoded;
        }
        to_p010.p = sws_getCachedContext(to_p010.p, decoded->width, decoded->height,
            static_cast<AVPixelFormat>(decoded->format), decoded->width, decoded->height,
            AV_PIX_FMT_P010, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (to_p010.p == nullptr) {
            fail("no ten-bit conversion from this pixel format");
        }
        av_frame_unref(p010_frame.p);
        p010_frame.p->format = AV_PIX_FMT_P010;
        p010_frame.p->width = decoded->width;
        p010_frame.p->height = decoded->height;
        check(av_frame_get_buffer(p010_frame.p, 0), "allocate P010 frame");
        sws_scale(to_p010.p, decoded->data, decoded->linesize, 0, decoded->height,
            p010_frame.p->data, p010_frame.p->linesize);
        // The timestamps travel with the pixels; everything downstream reads them off the
        // frame it was handed.
        p010_frame.p->pts = decoded->pts;
        p010_frame.p->best_effort_timestamp = decoded->best_effort_timestamp;
        return p010_frame.p;
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

        const Status s = SubmitAvFrame(enc, as_encoder_wants(dec_frame.p), meta);
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
    // The end of an encode, and what carries the report on sidecar records whose packet
    // never arrived. Session::finish does the same, in the same order: the warning has to
    // reach the caller before the sink says the stream is complete.
    if (const Status stopped = enc.Stop(); !stopped.ok()) {
        fail(stopped.message());
    }
    open_mux();
    file_sink.end();

    Report report = enc.ToReport();
    report.sidecar_path = sidecar_path;
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

} // namespace ta_roienc
