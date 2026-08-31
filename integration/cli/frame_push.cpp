// Drives sve::Session from a file: demuxes, then pushes frames in rather than
// handing the path to the file pipeline. Two reasons it exists. It is the only
// way to exercise the frame mode without a gRPC client in between, and running
// it against roi-transcode on the same input is the check that both modes agree.

#include "sve/sve.h"
#include "roi_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

namespace {

// AVPixelFormat to the library's own PixelFormat; the library does not know FFmpeg's
// numbering, and this tool is on the FFmpeg side of that line.
sve::PixelFormat to_library_format(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return sve::PixelFormat::YUV420P;
    case AV_PIX_FMT_NV12:    return sve::PixelFormat::NV12;
    case AV_PIX_FMT_YUV422P: return sve::PixelFormat::YUV422P;
    case AV_PIX_FMT_YUV444P: return sve::PixelFormat::YUV444P;
    case AV_PIX_FMT_GRAY8:   return sve::PixelFormat::GRAY8;
    default:                 return sve::PixelFormat::Unknown;
    }
}

void usage()
{
    std::fprintf(stderr,
        "Usage: roi-frame-push --in CLIP --roi roi.json --out OUT.mp4\n"
        "         [--push raw|bitstream] [--mask]\n"
        "         [--encoder auto|qsv|vaapi|nvenc|x264|x265|cpu]\n"
        "         [--codec h265|h264]\n"
        "         [--crf 28] [--gop N] [--preset balanced]\n"
        "\n"
        "Pushes frames into sve::Session instead of transcoding a path.\n"
        "  --push raw        decode here, submit raw planes (default)\n"
        "  --push bitstream  submit compressed access units, Session decodes\n"
        "  --mask            rasterize the boxes into a grayscale mask\n");
}

sve::InputCodec codec_of(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return sve::InputCodec::H264;
    case AV_CODEC_ID_HEVC: return sve::InputCodec::H265;
    case AV_CODEC_ID_MJPEG: return sve::InputCodec::Jpeg;
    default:
        throw std::runtime_error(
            "--push bitstream needs H.264, H.265 or JPEG input");
    }
}

// Boxes live on the timeline; a submitted frame wants the ones around its own
// instant. Same hold window the file pipeline applies.
std::vector<sve::Box> boxes_at(
    const std::vector<sve::Box>& boxes, double hold_sec, double t_sec)
{
    const double hold = hold_sec > 0 ? hold_sec : 0.25;
    std::vector<sve::Box> out;
    for (const auto& b : boxes) {
        if (std::abs(b.t_sec - t_sec) <= hold) {
            out.push_back(b);
        }
    }
    return out;
}

void rasterize(std::vector<uint8_t>& mask, int w, int h,
    const std::vector<sve::Box>& boxes)
{
    std::fill(mask.begin(), mask.end(), static_cast<uint8_t>(0));
    for (const auto& s : boxes) {
        const int x0 = std::clamp(static_cast<int>(std::lround(s.x * w)), 0, w);
        const int y0 = std::clamp(static_cast<int>(std::lround(s.y * h)), 0, h);
        const int x1 = std::clamp(
            static_cast<int>(std::lround((s.x + s.width) * w)), 0, w);
        const int y1 = std::clamp(
            static_cast<int>(std::lround((s.y + s.height) * h)), 0, h);
        for (int y = y0; y < y1; ++y) {
            std::fill(mask.begin() + static_cast<size_t>(y) * w + x0,
                mask.begin() + static_cast<size_t>(y) * w + x1,
                static_cast<uint8_t>(255));
        }
    }
}

struct Args {
    std::string in;
    std::string out;
    RoiJson roi_doc;
    bool have_roi = false;
    bool bitstream = false;
    bool use_mask = false;
    sve::Encoder encoder = sve::Encoder::Auto;
    sve::BackendSelection backend_selection;
    sve::Codec codec = sve::Codec::H265;
    int crf = 28;
    int gop = 0;
    std::string preset = "balanced";
};

int run(const Args& args)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, args.in.c_str(), nullptr, nullptr) < 0) {
        throw std::runtime_error("open input");
    }
    struct Closer {
        AVFormatContext** p;
        ~Closer() { avformat_close_input(p); }
    } closer{&fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        throw std::runtime_error("find stream info");
    }
    int vindex = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vindex = static_cast<int>(i);
            break;
        }
    }
    if (vindex < 0) {
        throw std::runtime_error("no video stream");
    }
    AVStream* vs = fmt->streams[vindex];

    sve::SessionParams sp;
    sp.width = vs->codecpar->width;
    sp.height = vs->codecpar->height;
    sp.time_base_num = vs->time_base.num;
    sp.time_base_den = vs->time_base.den;
    if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0) {
        sp.fps_num = vs->avg_frame_rate.num;
        sp.fps_den = vs->avg_frame_rate.den;
    }
    sp.settings.encoder = args.encoder;
    // Same as the transcode CLI: --encoder left unsaid means this application's own order,
    // because the library no longer has one to fall back on (§8.4).
    sp.settings.backend_selection = args.backend_selection.preferred.empty()
        ? sve::encoder_choice_from_name("auto").selection
        : args.backend_selection;
    sp.settings.codec = args.codec;
    sp.settings.crf = args.crf;
    sp.settings.gop = args.gop;
    sp.settings.preset = sve::preset_from_name(args.preset.c_str());
    sp.settings.quality = args.roi_doc.quality;
    // A rasterized box is on or off, so two levels is all the mask can express.
    if (args.use_mask) {
        sp.settings.quality.mask_levels = 2;
    }

    if (args.bitstream) {
        sp.codec = codec_of(vs->codecpar->codec_id);
        if (vs->codecpar->extradata && vs->codecpar->extradata_size > 0) {
            sp.extradata.assign(vs->codecpar->extradata,
                vs->codecpar->extradata + vs->codecpar->extradata_size);
        }
    }

    sp.output_path = args.out;
    sve::Session session(sp);

    std::vector<uint8_t> mask_buf(
        static_cast<size_t>(sp.width) * static_cast<size_t>(sp.height));
    sve::Mask mask;
    mask.data = mask_buf;
    mask.width = sp.width;
    mask.height = sp.height;
    mask.stride = sp.width;

    auto regions_for = [&](double t_sec, std::vector<sve::Box>& keep) {
        keep = boxes_at(args.roi_doc.boxes, args.roi_doc.quality.hold_sec, t_sec);
        sve::Regions r;
        if (args.use_mask) {
            rasterize(mask_buf, sp.width, sp.height, keep);
            r.mask = mask;
        } else if (!keep.empty()) {
            r.boxes = keep;
        }
        return r;
    };

    AVPacket* pkt = av_packet_alloc();
    struct PktFree {
        AVPacket** p;
        ~PktFree() { av_packet_free(p); }
    } pkt_free{&pkt};

    if (args.bitstream) {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vindex) {
                const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : 0;
                const double t = static_cast<double>(pts) * av_q2d(vs->time_base);
                std::vector<sve::Box> keep;
                const sve::Regions r = regions_for(t, keep);
                session.submit(
                    std::span<const uint8_t>(
                        pkt->data, static_cast<size_t>(pkt->size)),
                    r, pts);
            }
            av_packet_unref(pkt);
        }
    } else {
        const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!dec) {
            throw std::runtime_error("no decoder for input");
        }
        AVCodecContext* dc = avcodec_alloc_context3(dec);
        struct DecFree {
            AVCodecContext** p;
            ~DecFree() { avcodec_free_context(p); }
        } dec_free{&dc};
        if (!dc || avcodec_parameters_to_context(dc, vs->codecpar) < 0
            || avcodec_open2(dc, dec, nullptr) < 0) {
            throw std::runtime_error("open decoder");
        }

        AVFrame* frame = av_frame_alloc();
        struct FrameFree {
            AVFrame** p;
            ~FrameFree() { av_frame_free(p); }
        } frame_free{&frame};

        auto drain = [&]() {
            for (;;) {
                const int r = avcodec_receive_frame(dc, frame);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                    return;
                }
                if (r < 0) {
                    throw std::runtime_error("receive frame");
                }
                const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp
                    : 0;
                const double t = static_cast<double>(pts) * av_q2d(vs->time_base);
                std::vector<sve::Box> keep;
                const sve::Regions rg = regions_for(t, keep);
                sve::PlaneBuffer::Plane planes[4];
                for (int i = 0; i < 4; ++i) {
                    if (!frame->data[i] || frame->linesize[i] <= 0) {
                        continue;
                    }
                    // Chroma planes are shorter than the frame by the format's vertical
                    // subsampling; claiming full height would hand the library a bound
                    // wider than the buffer.
                    int rows = frame->height;
                    if (i > 0) {
                        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(
                            static_cast<AVPixelFormat>(frame->format));
                        const int shift = d ? d->log2_chroma_h : 0;
                        rows = (frame->height + (1 << shift) - 1) >> shift;
                    }
                    planes[i] = {std::span<uint8_t>(frame->data[i],
                                     static_cast<size_t>(frame->linesize[i])
                                         * static_cast<size_t>(rows)),
                        frame->linesize[i]};
                }
                sve::PlaneBuffer pixels(frame->width, frame->height,
                    to_library_format(static_cast<AVPixelFormat>(frame->format)),
                    std::span<const sve::PlaneBuffer::Plane>(planes, 4));
                session.submit(pixels, rg, pts);
                av_frame_unref(frame);
            }
        };

        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vindex) {
                if (avcodec_send_packet(dc, pkt) < 0) {
                    throw std::runtime_error("send packet");
                }
                drain();
            }
            av_packet_unref(pkt);
        }
        avcodec_send_packet(dc, nullptr);
        drain();
    }

    const sve::Report rep = session.finish();
    std::fprintf(stderr,
        "pushed %lld frames -> %lld packets, %lld bytes, %s%s\n",
        static_cast<long long>(rep.frames), static_cast<long long>(rep.packets),
        static_cast<long long>(rep.bytes), rep.encoder_name.c_str(),
        args.use_mask ? " (mask)" : " (boxes)");
    if (rep.regions_dropped > 0) {
        std::fprintf(stderr, "warning: %lld regions over the budget were dropped\n",
            static_cast<long long>(rep.regions_dropped));
    }
    if (rep.roi_lag_measured) {
        std::fprintf(stderr,
            "roi lag: samples=%lld min_iou=%.3f p95_frames=%.2f p99_frames=%.2f "
            "p95_ms=%.1f p99_ms=%.1f acceptance=%s\n",
            static_cast<long long>(rep.roi_lag_samples), rep.roi_lag_min_iou,
            rep.roi_lag_p95_frames, rep.roi_lag_p99_frames, rep.roi_lag_p95_ms,
            rep.roi_lag_p99_ms, rep.roi_lag_acceptance ? "pass" : "fail");
    } else {
        std::fprintf(stderr, "roi lag: not measured (no tracked boxes)\n");
    }
    return 0;
}

sve::Codec parse_codec(const char* s)
{
    if (std::strcmp(s, "h264") == 0) return sve::Codec::H264;
    if (std::strcmp(s, "h265") == 0 || std::strcmp(s, "hevc") == 0)
        return sve::Codec::H265;
    throw std::runtime_error("unknown --codec (h264|h265)");
}
} // namespace

int main(int argc, char** argv)
{
    Args args;
    try {
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc) {
                    throw std::runtime_error(
                        std::string("missing value for ") + name);
                }
                return argv[++i];
            };
            if (std::strcmp(a, "--in") == 0) {
                args.in = need("--in");
            } else if (std::strcmp(a, "--out") == 0) {
                args.out = need("--out");
            } else if (std::strcmp(a, "--roi") == 0) {
                args.roi_doc = load_roi_json(need("--roi"));
                args.have_roi = true;
            } else if (std::strcmp(a, "--push") == 0) {
                const char* mode = need("--push");
                if (std::strcmp(mode, "raw") == 0) {
                    args.bitstream = false;
                } else if (std::strcmp(mode, "bitstream") == 0) {
                    args.bitstream = true;
                } else {
                    throw std::runtime_error("--push takes raw|bitstream");
                }
            } else if (std::strcmp(a, "--mask") == 0) {
                args.use_mask = true;
            } else if (std::strcmp(a, "--encoder") == 0) {
                const sve::EncoderChoice choice =
                    sve::encoder_choice_from_name(need("--encoder"));
                args.encoder = choice.kind;
                args.backend_selection = choice.selection;
            } else if (std::strcmp(a, "--codec") == 0) {
                args.codec = parse_codec(need("--codec"));
            } else if (std::strcmp(a, "--crf") == 0) {
                args.crf = std::stoi(need("--crf"));
            } else if (std::strcmp(a, "--gop") == 0) {
                args.gop = std::stoi(need("--gop"));
            } else if (std::strcmp(a, "--preset") == 0) {
                args.preset = need("--preset");
            } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
                usage();
                return 0;
            } else {
                throw std::runtime_error(std::string("unknown arg: ") + a);
            }
        }
        if (args.in.empty() || args.out.empty() || !args.have_roi) {
            usage();
            return 2;
        }
        return run(args);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
