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
//                    [--baseline <same-clip-encoded-without-regions.mp4>]
//
// --baseline turns the §44 boundary zones from absolute numbers into a differential, which
// is the only form in which they mean anything; see paint_zones.

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

/// A perfect match has no finite PSNR. This stands in for "identical" so an average over
/// frames stays a number; it is well above anything a real encode reaches.
constexpr double PSNR_IDENTICAL = 100.0;

double psnr_from_mse(double mse)
{
    return mse <= 1e-9 ? PSNR_IDENTICAL : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// Zone ids for §44, from outside inwards. NONE is everything far from any edge: the open
/// background and the deep interior of a large object, neither of which says anything
/// about the boundary.
enum Zone : int8_t {
    ZONE_NONE = -1,
    ZONE_OUTSIDE = 0,
    ZONE_BOUNDARY_OUTER = 1,
    ZONE_BOUNDARY = 2,
    ZONE_BOUNDARY_INNER = 3,
    ZONE_INTERIOR = 4,
};

/// Bands are one coding block wide. Narrower and the zone would say more about the block
/// grid than about the edge.
constexpr int BAND = 8;
constexpr int HALF_BAND = BAND / 2;

/// Paints, once per frame, which band around a region edge each pixel falls in, and
/// separately whether it is inside a region at all.
///
/// The five bands are concentric and equally thick, spanning +-2 blocks around the edge:
///
///     outside | boundary_outer | boundary | boundary_inner | interior
///        -2B  ..  -B  ..  -B/2  ..  edge  ..  +B/2  ..  +B  ..  +2B
///
/// They are bands and not "everything outside" / "everything inside" on purpose. The first
/// version made ZONE_OUTSIDE the whole rest of the frame and ZONE_BOUNDARY a single pixel
/// row, and the numbers that came out were unreadable: the one-pixel ring sits exactly on
/// the object silhouette, the highest-detail pixels in the picture, so it scored ~13 dB
/// below its neighbours *in a baseline encode that treats the frame uniformly*. That is
/// content, not a frame around the region -- but it looks exactly like one. Comparable
/// bands, plus the --baseline differential below, are what separate the two.
///
/// The obvious implementation -- ask each pixel for its distance to every region -- costs
/// width x height x regions with a square root in the middle, which on a 720p clip of 300
/// frames is hundreds of millions of calls and made this tool unusable in CI. Only the
/// neighbourhood of a region can be in any band, so only that is walked, and a pixel
/// covered by two regions keeps the innermost zone.
void paint_zones(std::vector<int8_t>& zone, std::vector<uint8_t>& inside, int w, int h,
    const std::vector<Rect>& rois)
{
    const size_t px = static_cast<size_t>(w) * h;
    zone.assign(px, ZONE_NONE);
    inside.assign(px, 0);
    for (const Rect& r : rois) {
        const int reach = 2 * BAND;
        const int x0 = std::max(0, r.x - reach);
        const int x1 = std::min(w - 1, r.x + r.w - 1 + reach);
        const int y0 = std::max(0, r.y - reach);
        const int y1 = std::min(h - 1, r.y + r.h - 1 + reach);
        for (int y = y0; y <= y1; ++y) {
            int8_t* row = zone.data() + static_cast<size_t>(y) * w;
            uint8_t* in = inside.data() + static_cast<size_t>(y) * w;
            for (int x = x0; x <= x1; ++x) {
                const int dx = std::max({r.x - x, x - (r.x + r.w - 1), 0});
                const int dy = std::max({r.y - y, y - (r.y + r.h - 1), 0});
                // Signed Chebyshev distance to the edge: positive inside, and 0 on the
                // outermost row of the region itself. Chebyshev rather than Euclidean
                // because the bands are a ring around a rectangle, and a square ring is
                // what the encoder's blocks see.
                int d;
                if (dx == 0 && dy == 0) {
                    in[x] = 1;
                    d = std::min({x - r.x, r.x + r.w - 1 - x,
                                  y - r.y, r.y + r.h - 1 - y});
                } else {
                    d = -std::max(dx, dy);
                }
                int8_t z;
                if (d < -2 * BAND || d > 2 * BAND)  z = ZONE_NONE;
                else if (d < -BAND)                 z = ZONE_OUTSIDE;
                else if (d < -HALF_BAND)            z = ZONE_BOUNDARY_OUTER;
                else if (d <= HALF_BAND)            z = ZONE_BOUNDARY;
                else if (d <= BAND)                 z = ZONE_BOUNDARY_INNER;
                else                                z = ZONE_INTERIOR;
                row[x] = std::max(row[x], z);
            }
        }
    }
}

/// 8x8 windowed SSIM on luma, averaged over the windows whose centre `keep` accepts.
double ssim_masked(const Plane& a, const Plane& b,
    const std::vector<uint8_t>& inside, bool want_inside)
{
    constexpr int W = 8;
    const double c1 = 6.5025, c2 = 58.5225;
    double total = 0.0;
    long windows = 0;
    for (int y = 0; y + W <= a.h; y += 4) {
        for (int x = 0; x + W <= a.w; x += 4) {
            const int cx = x + W / 2, cy = y + W / 2;
            const bool is_inside = inside[static_cast<size_t>(cy) * a.w + cx] != 0;
            if (is_inside != want_inside) {
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


} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> pos;
    std::string baseline_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--baseline" && i + 1 < argc) {
            baseline_path = argv[++i];
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() < 3) {
        std::fprintf(stderr,
            "usage: roi_metrics <reference> <distorted> <truth.json> [label] "
            "[--baseline <encode>]\n");
        return 2;
    }
    const std::string label = pos.size() > 3 ? pos[3] : "run";

    std::vector<Plane> ref, dis, base;
    if (!decode_luma(pos[0].c_str(), ref) || !decode_luma(pos[1].c_str(), dis)) {
        return 1;
    }
    size_t n = std::min(ref.size(), dis.size());
    if (n == 0 || ref[0].w != dis[0].w || ref[0].h != dis[0].h) {
        std::fprintf(stderr, "reference and distorted differ in size\n");
        return 1;
    }
    if (!baseline_path.empty()) {
        if (!decode_luma(baseline_path.c_str(), base)) {
            return 1;
        }
        if (base.empty() || base[0].w != ref[0].w || base[0].h != ref[0].h) {
            std::fprintf(stderr, "baseline differs in size from the reference\n");
            return 1;
        }
        n = std::min(n, base.size());
    }

    nlohmann::json truth;
    {
        FILE* f = std::fopen(pos[2].c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", pos[2].c_str());
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

    std::vector<int8_t> zone;
    std::vector<uint8_t> inside;
    std::vector<double> roi_psnr_series, bg_psnr_series;
    double roi_ssim_sum = 0, bg_ssim_sum = 0;
    long roi_frames = 0;
    // §44: outside, boundary-outer, boundary, boundary-inner, interior.
    double zone_mse[5] = {0, 0, 0, 0, 0};
    double base_zone_mse[5] = {0, 0, 0, 0, 0};
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
        const Plane* c = base.empty() ? nullptr : &base[fi];
        paint_zones(zone, inside, a.w, a.h, rois);

        double roi_se = 0, bg_se = 0;
        long roi_px = 0, bg_px = 0;

        for (int y = 0; y < a.h; ++y) {
            const uint8_t* ra = a.at(y);
            const uint8_t* rb = b.at(y);
            const uint8_t* rc = c ? c->at(y) : nullptr;
            const int8_t* rz = zone.data() + static_cast<size_t>(y) * a.w;
            const uint8_t* ri = inside.data() + static_cast<size_t>(y) * a.w;
            for (int x = 0; x < a.w; ++x) {
                const double d = static_cast<double>(ra[x]) - rb[x];
                const double se = d * d;
                // Inside means the region proper -- the edge counts as inside, because a
                // pixel on the border of a plate is part of the plate.
                if (ri[x]) {
                    roi_se += se;
                    ++roi_px;
                } else {
                    bg_se += se;
                    ++bg_px;
                }
                const int8_t z = rz[x];
                if (z != ZONE_NONE) {
                    zone_mse[z] += se;
                    ++zone_px[z];
                    if (rc) {
                        const double db = static_cast<double>(ra[x]) - rc[x];
                        base_zone_mse[z] += db * db;
                    }
                }
            }
        }

        if (roi_px > 0) {
            roi_psnr_series.push_back(psnr_from_mse(roi_se / static_cast<double>(roi_px)));
            roi_ssim_sum += ssim_masked(a, b, inside, true);
            ++roi_frames;
        }
        if (bg_px > 0) {
            bg_psnr_series.push_back(psnr_from_mse(bg_se / static_cast<double>(bg_px)));
            bg_ssim_sum += ssim_masked(a, b, inside, false);
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
    double gain[5] = {0, 0, 0, 0, 0};
    bool have_gain = !base.empty();
    for (int z = 0; z < 5; ++z) {
        if (zone_px[z] == 0) {
            have_gain = false;
            continue;
        }
        const double n_px = static_cast<double>(zone_px[z]);
        const double q = psnr_from_mse(zone_mse[z] / n_px);
        if (base.empty()) {
            std::printf("  zone_%s_psnr=%.3f\n", zone_name[z], q);
            continue;
        }
        const double qb = psnr_from_mse(base_zone_mse[z] / n_px);
        // A band either encode reproduced exactly has no gain to report: subtracting the
        // stand-in would print a difference of tens of dB that means only "both were
        // lossless here". Flat synthetic scenes hit this, so it is not a corner case.
        if (q >= PSNR_IDENTICAL || qb >= PSNR_IDENTICAL) {
            std::printf("  zone_%s_psnr=%.3f  zone_%s_gain=lossless\n",
                zone_name[z], q, zone_name[z]);
            have_gain = false;
            continue;
        }
        gain[z] = q - qb;
        std::printf("  zone_%s_psnr=%.3f  zone_%s_gain=%+.3f\n",
            zone_name[z], q, zone_name[z], gain[z]);
    }

    // §44 asks whether the region shows a visible frame. Absolute PSNR per band cannot
    // answer that -- the bands hold different content, and the one on the silhouette is
    // the hardest to code no matter how it is encoded. What a frame actually is, is a ring
    // that came off *worse than the rings on both sides of it* once the region handling
    // was switched on. So the statistic is the deepest local dip in the gain profile, and
    // it needs the same clip encoded without regions to exist at all.
    if (have_gain) {
        double dip = 0.0;
        int at = -1;
        for (int z = 1; z <= 3; ++z) {
            const double d = std::min(gain[z - 1], gain[z + 1]) - gain[z];
            if (d > dip) {
                dip = d;
                at = z;
            }
        }
        std::printf("  boundary_dip=%.3f  at=%s\n", dip,
            at < 0 ? "none" : zone_name[at]);
    }
    return 0;
}
