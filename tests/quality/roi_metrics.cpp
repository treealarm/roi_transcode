// Quality inside the regions against quality outside them -- TZ v2.0 §40, §43, §44.
//
// The pair is the point. §40 says plainly that a global score cannot be the KPI, because
// an adaptive encoder degrades the background on purpose: a single number cannot tell
// "protected the plate and spent the savings on the sky" from "made everything worse".
// So every metric here is reported three times -- ROI, background, whole frame -- and the
// interesting quantity is the gap.
//
// PSNR and SSIM rather than VMAF for now: both are exact, need no model and no extra
// dependency, and answer the question the acceptance criteria ask. VMAF layers on top
// once libvmaf is in the build.
//
// Usage: roi_metrics <reference.mp4> <distorted.mp4> <truth.json> [label]

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

/// One decoded luma plane, tightly packed.
struct Plane {
    int w = 0, h = 0;
    std::vector<uint8_t> y;
    const uint8_t* at(int row) const { return y.data() + static_cast<size_t>(row) * w; }
};

/// Decodes every frame's Y plane. These are synthetic clips of a few hundred frames, so
/// holding them is simpler than streaming two decoders in lockstep and cannot desynchronise.
bool decode_luma(const char* path, std::vector<Plane>& out)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    int vi = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vi = static_cast<int>(i);
            break;
        }
    }
    if (vi < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vi]->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    auto take = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            Plane p;
            p.w = frame->width;
            p.h = frame->height;
            p.y.resize(static_cast<size_t>(p.w) * p.h);
            if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_NV12) {
                for (int r = 0; r < p.h; ++r) {
                    std::memcpy(p.y.data() + static_cast<size_t>(r) * p.w,
                        frame->data[0] + static_cast<size_t>(r) * frame->linesize[0], p.w);
                }
            } else {
                // Anything else is converted rather than refused: the reference and the
                // distorted clip need not have come out of the same encoder.
                sws = sws_getCachedContext(sws, p.w, p.h,
                    static_cast<AVPixelFormat>(frame->format), p.w, p.h,
                    AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
                uint8_t* dst[4] = {p.y.data(), nullptr, nullptr, nullptr};
                int stride[4] = {p.w, 0, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, p.h, dst, stride);
            }
            out.push_back(std::move(p));
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vi && avcodec_send_packet(ctx, pkt) == 0) {
            take();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    take();

    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return !out.empty();
}

double psnr_from_mse(double mse)
{
    // A perfect match has no finite PSNR. 100 stands in for "identical" so an average over
    // frames stays a number; it is well above anything a real encode reaches.
    return mse <= 1e-9 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// 8x8 windowed SSIM on luma, averaged over the windows whose centre `keep` accepts.
double ssim_masked(const Plane& a, const Plane& b,
    const std::function<bool(int, int)>& keep)
{
    constexpr int W = 8;
    const double c1 = 6.5025, c2 = 58.5225;
    double total = 0.0;
    long windows = 0;
    for (int y = 0; y + W <= a.h; y += 4) {
        for (int x = 0; x + W <= a.w; x += 4) {
            if (!keep(x + W / 2, y + W / 2)) {
                continue;
            }
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int j = 0; j < W; ++j) {
                const uint8_t* ra = a.at(y + j) + x;
                const uint8_t* rb = b.at(y + j) + x;
                for (int i = 0; i < W; ++i) {
                    const double va = ra[i], vb = rb[i];
                    sa += va; sb += vb;
                    saa += va * va; sbb += vb * vb; sab += va * vb;
                }
            }
            const double n = W * W;
            const double ma = sa / n, mb = sb / n;
            const double va = saa / n - ma * ma;
            const double vb = sbb / n - mb * mb;
            const double cov = sab / n - ma * mb;
            total += ((2 * ma * mb + c1) * (2 * cov + c2))
                / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++windows;
        }
    }
    return windows > 0 ? total / static_cast<double>(windows) : 0.0;
}

/// Distance from the nearest ROI edge, negative inside. Used for the boundary zones of §44.
int signed_distance(const std::vector<Rect>& rois, int x, int y)
{
    int best = std::numeric_limits<int>::max();
    bool inside = false;
    for (const Rect& r : rois) {
        const int dx = std::max({r.x - x, x - (r.x + r.w - 1), 0});
        const int dy = std::max({r.y - y, y - (r.y + r.h - 1), 0});
        if (dx == 0 && dy == 0) {
            inside = true;
            const int to_edge = std::min({x - r.x, r.x + r.w - 1 - x,
                                          y - r.y, r.y + r.h - 1 - y});
            best = std::min(best, -to_edge);
        } else if (!inside) {
            best = std::min(best, static_cast<int>(std::hypot(dx, dy)));
        }
    }
    return rois.empty() ? std::numeric_limits<int>::max() : best;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: roi_metrics <reference> <distorted> <truth.json> [label]\n");
        return 2;
    }
    const std::string label = argc > 4 ? argv[4] : "run";

    std::vector<Plane> ref, dis;
    if (!decode_luma(argv[1], ref) || !decode_luma(argv[2], dis)) {
        return 1;
    }
    const size_t n = std::min(ref.size(), dis.size());
    if (n == 0 || ref[0].w != dis[0].w || ref[0].h != dis[0].h) {
        std::fprintf(stderr, "reference and distorted differ in size\n");
        return 1;
    }

    nlohmann::json truth;
    {
        FILE* f = std::fopen(argv[3], "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[3]);
            return 1;
        }
        std::string buf;
        char chunk[8192];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, got);
        std::fclose(f);
        truth = nlohmann::json::parse(buf, nullptr, false);
        if (truth.is_discarded()) {
            std::fprintf(stderr, "truth.json is not valid JSON\n");
            return 1;
        }
    }

    std::vector<double> roi_psnr_series, bg_psnr_series;
    double roi_ssim_sum = 0, bg_ssim_sum = 0;
    long roi_frames = 0;
    // §44: outside, boundary-outer, boundary, boundary-inner, interior.
    double zone_mse[5] = {0, 0, 0, 0, 0};
    long zone_px[5] = {0, 0, 0, 0, 0};

    for (size_t fi = 0; fi < n; ++fi) {
        std::vector<Rect> rois;
        if (fi < truth["frames"].size()) {
            for (const auto& o : truth["frames"][fi]["objects"]) {
                const auto& b = o["bbox"];
                rois.push_back({b[0].get<int>(), b[1].get<int>(),
                                b[2].get<int>(), b[3].get<int>()});
            }
        }

        const Plane& a = ref[fi];
        const Plane& b = dis[fi];
        double roi_se = 0, bg_se = 0;
        long roi_px = 0, bg_px = 0;

        for (int y = 0; y < a.h; ++y) {
            const uint8_t* ra = a.at(y);
            const uint8_t* rb = b.at(y);
            for (int x = 0; x < a.w; ++x) {
                const double d = static_cast<double>(ra[x]) - rb[x];
                const double se = d * d;
                const int sd = signed_distance(rois, x, y);
                if (sd <= 0) {
                    roi_se += se;
                    ++roi_px;
                } else {
                    bg_se += se;
                    ++bg_px;
                }
                // Zones are eight pixels wide, which is one coding block: narrower than
                // that and the zone says more about the block grid than about the edge.
                int z;
                if (sd > 8)        z = 0;
                else if (sd > 0)   z = 1;
                else if (sd == 0)  z = 2;
                else if (sd > -8)  z = 3;
                else               z = 4;
                zone_mse[z] += se;
                ++zone_px[z];
            }
        }

        if (roi_px > 0) {
            roi_psnr_series.push_back(psnr_from_mse(roi_se / static_cast<double>(roi_px)));
            roi_ssim_sum += ssim_masked(a, b,
                [&](int x, int y) { return signed_distance(rois, x, y) <= 0; });
            ++roi_frames;
        }
        if (bg_px > 0) {
            bg_psnr_series.push_back(psnr_from_mse(bg_se / static_cast<double>(bg_px)));
            bg_ssim_sum += ssim_masked(a, b,
                [&](int x, int y) { return signed_distance(rois, x, y) > 0; });
        }
    }

    auto mean = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    };
    // §43: mean |Q(t) - Q(t-1)|. Flicker, not absolute quality -- a stream can sit at a
    // fine average and still pulse visibly, which is exactly what temporal ROI is meant
    // to stop and what nothing else here would catch.
    auto instability = [](const std::vector<double>& v) {
        if (v.size() < 2) return 0.0;
        double s = 0;
        for (size_t i = 1; i < v.size(); ++i) s += std::fabs(v[i] - v[i - 1]);
        return s / static_cast<double>(v.size() - 1);
    };

    const double roi_psnr = mean(roi_psnr_series);
    const double bg_psnr = mean(bg_psnr_series);

    std::printf("label=%s frames=%zu\n", label.c_str(), n);
    std::printf("  roi_psnr=%.3f  bg_psnr=%.3f  separation=%.3f\n",
        roi_psnr, bg_psnr, roi_psnr - bg_psnr);
    std::printf("  roi_ssim=%.5f  bg_ssim=%.5f\n",
        roi_frames ? roi_ssim_sum / roi_frames : 0.0,
        roi_frames ? bg_ssim_sum / roi_frames : 0.0);
    std::printf("  roi_instability=%.4f  bg_instability=%.4f\n",
        instability(roi_psnr_series), instability(bg_psnr_series));
    static const char* zone_name[5] = {
        "outside", "boundary_outer", "boundary", "boundary_inner", "interior"};
    for (int z = 0; z < 5; ++z) {
        if (zone_px[z] > 0) {
            std::printf("  zone_%s_psnr=%.3f\n", zone_name[z],
                psnr_from_mse(zone_mse[z] / static_cast<double>(zone_px[z])));
        }
    }
    return 0;
}
