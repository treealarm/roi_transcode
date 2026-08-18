// Drives roi::Session from a file: demuxes, then pushes frames in rather than
// handing the path to the file pipeline. Two reasons it exists. It is the only
// way to exercise the frame mode without a gRPC client in between, and running
// it against roi-transcode on the same input is the check that both modes agree.

#include "roi/roi.h"
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

void usage()
{
    std::fprintf(stderr,
        "Usage: roi-frame-push --in CLIP --roi roi.json --out OUT.mp4\n"
        "         [--push raw|bitstream] [--mask] [--encoder auto|qsv|vaapi|nvenc|x265]\n"
        "         [--crf 28] [--gop N] [--preset medium]\n"
        "\n"
        "Pushes frames into roi::Session instead of transcoding a path.\n"
        "  --push raw        decode here, submit raw planes (default)\n"
        "  --push bitstream  submit compressed access units, Session decodes\n"
        "  --mask            rasterize the boxes into a grayscale mask\n");
}

roi::Encoder parse_encoder(const char* s)
{
    if (std::strcmp(s, "auto") == 0) return roi::Encoder::Auto;
    if (std::strcmp(s, "qsv") == 0) return roi::Encoder::Qsv;
    if (std::strcmp(s, "vaapi") == 0) return roi::Encoder::Vaapi;
    if (std::strcmp(s, "nvenc") == 0) return roi::Encoder::Nvenc;
    if (std::strcmp(s, "x265") == 0) return roi::Encoder::X265;
    throw std::runtime_error("unknown --encoder (auto|qsv|vaapi|nvenc|x265)");
}

roi::InputCodec codec_of(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return roi::InputCodec::H264;
    case AV_CODEC_ID_HEVC: return roi::InputCodec::H265;
    case AV_CODEC_ID_MJPEG: return roi::InputCodec::Jpeg;
    default:
        throw std::runtime_error(
            "--push bitstream needs H.264, H.265 or JPEG input");
    }
}

// Boxes live on the timeline; a submitted frame wants the ones around its own
// instant. Same hold window the file pipeline applies.
std::vector<roi::Box> boxes_at(
    const std::vector<roi::Box>& boxes, double hold_sec, double t_sec)
{
    const double hold = hold_sec > 0 ? hold_sec : 0.25;
    std::vector<roi::Box> out;
    for (const auto& b : boxes) {
        if (std::abs(b.t_sec - t_sec) <= hold) {
            out.push_back(b);
        }
    }
    return out;
}

void rasterize(std::vector<uint8_t>& mask, int w, int h,
    const std::vector<roi::Box>& boxes)
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
    roi::Encoder encoder = roi::Encoder::Auto;
    int crf = 28;
    int gop = 0;
    std::string preset = "medium";
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

    roi::SessionParams sp;
    sp.width = vs->codecpar->width;
    sp.height = vs->codecpar->height;
    sp.time_base_num = vs->time_base.num;
    sp.time_base_den = vs->time_base.den;
    if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0) {
        sp.fps_num = vs->avg_frame_rate.num;
        sp.fps_den = vs->avg_frame_rate.den;
    }
    sp.settings.encoder = args.encoder;
    sp.settings.crf = args.crf;
    sp.settings.gop = args.gop;
    sp.settings.preset = args.preset;
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
    roi::Session session(sp);

    std::vector<uint8_t> mask_buf(
        static_cast<size_t>(sp.width) * static_cast<size_t>(sp.height));
    roi::Mask mask;
    mask.data = mask_buf.data();
    mask.width = sp.width;
    mask.height = sp.height;
    mask.stride = sp.width;

    auto regions_for = [&](double t_sec, std::vector<roi::Box>& keep) {
        keep = boxes_at(args.roi_doc.boxes, args.roi_doc.quality.hold_sec, t_sec);
        roi::Regions r;
        if (args.use_mask) {
            rasterize(mask_buf, sp.width, sp.height, keep);
            r.mask = &mask;
        } else if (!keep.empty()) {
            r.boxes = keep.data();
            r.box_count = keep.size();
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
                std::vector<roi::Box> keep;
                const roi::Regions r = regions_for(t, keep);
                roi::Frame f;
                f.encoded = pkt->data;
                f.encoded_size = static_cast<size_t>(pkt->size);
                f.pts = pts;
                session.submit(f, r);
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
                std::vector<roi::Box> keep;
                const roi::Regions rg = regions_for(t, keep);
                roi::Frame f;
                for (int i = 0; i < 4; ++i) {
                    f.plane[i] = frame->data[i];
                    f.stride[i] = frame->linesize[i];
                }
                f.pix_fmt = frame->format;
                f.width = frame->width;
                f.height = frame->height;
                f.pts = pts;
                session.submit(f, rg);
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

    const roi::Report rep = session.finish();
    std::fprintf(stderr,
        "pushed %lld frames -> %lld packets, %lld bytes, %s%s\n",
        static_cast<long long>(rep.frames), static_cast<long long>(rep.packets),
        static_cast<long long>(rep.bytes), rep.encoder_name.c_str(),
        args.use_mask ? " (mask)" : " (boxes)");
    if (rep.regions_dropped > 0) {
        std::fprintf(stderr, "warning: %lld regions over the budget were dropped\n",
            static_cast<long long>(rep.regions_dropped));
    }
    return 0;
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
                args.encoder = parse_encoder(need("--encoder"));
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
