// A consumer's view: one include, one namespace, both entry points. If this
// stops compiling, the public surface has grown a dependency it should not have.
//
// It is a compile test with a runtime tail: transcode_file and Session are only
// named here, not driven, because exercising them needs media. What it proves is
// the shape -- that "sve/sve.h" alone is enough to write against.

#include "sve/sve.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Everything a caller needs to say, said with the public vocabulary only.
sve::Settings make_settings()
{
    sve::Settings s;
    s.encoder = sve::Encoder::Auto;
    s.crf = 28;
    s.gop = 1;
    s.preset = sve::Preset::Balanced;
    s.quality.background_qp_delta = 20;
    s.quality.hold_sec = 0.5;
    s.quality.max_regions = 32;
    s.quality.kinds["plate"] = sve::KindQuality{-8, 0.01};
    s.quality.mask_qp_delta_at_255 = -8;
    s.quality.mask_levels = 2;
    return s;
}

std::vector<sve::Box> make_boxes()
{
    return {sve::Box{
        .t_sec = 1.25,
        .kind = "plate",
        .x = 0.35f,
        .y = 0.40f,
        .width = 0.30f,
        .height = 0.20f,
        .label = "",
        .polygon = {},
        .track = {},
    }};
}

// Receiving packets is part of the surface too -- and it is FFmpeg-free, which
// is why this file compiles without libav* anywhere on its include path.
class CountingHandler : public sve::PacketHandler {
public:
    void on_open(const sve::StreamInfo& info) override
    {
        width = info.width;
        name = info.encoder_name;
    }
    void on_packet(const sve::EncodedPacket& p) override
    {
        ++packets;
        bytes += p.data.size();
    }
    void on_end() override { ++ends; }

    int width = 0;
    std::string name;
    int packets = 0;
    std::size_t bytes = 0;
    int ends = 0;
};

// §7's interface, named with the public vocabulary alone. This is the part that keeps the
// interface honest: BackendSession is opaque here, so an AVCodecContext creeping into the
// public header would stop this file compiling -- which is the point of compiling it
// without libav* on the include path at all.
[[maybe_unused]] void names_the_backend_interface()
{
    for (const sve::IEncoderBackend& b :
        sve::backend_chain(sve::Encoder::Cpu)) {
        const sve::EncoderCapabilities named = b.GetCapabilities(
            sve::Codec::H264);
        (void)named.h264;
    }
    const sve::EncoderCapabilities caps =
        sve::backend_chain(sve::Encoder::Cpu)[0].get().GetCapabilities(
            sve::Codec::H264);
    (void)caps.h264;
    (void)caps.qp_map;
    (void)caps.qp_map_block_width;
    (void)caps.qp_map_block_height;
    (void)caps.sei;
    (void)caps.native_bitrate;
    (void)caps.max_roi_regions;
    if (!caps.qp_map || caps.qp_map_block_width != 16 || caps.qp_map_block_height != 16) {
        std::fprintf(stderr, "fail: Cpu H.264 must advertise qp_map 16x16\n");
        std::exit(1);
    }
    if (!caps.sei) {
        std::fprintf(stderr, "fail: Cpu must advertise sei\n");
        std::exit(1);
    }
    if (!caps.native_bitrate) {
        std::fprintf(stderr, "fail: Cpu must advertise native_bitrate\n");
        std::exit(1);
    }
    const sve::EncoderCapabilities hevc =
        sve::backend_chain(sve::Encoder::Cpu)[0].get().GetCapabilities(
            sve::Codec::H265);
    if (!hevc.qp_map || hevc.qp_map_block_width != 32) {
        std::fprintf(stderr, "fail: Cpu H.265 must advertise qp_map 32x32\n");
        std::exit(1);
    }
    if (hevc.native_bitrate) {
        std::fprintf(stderr, "fail: Cpu H.265 must keep native_bitrate off\n");
        std::exit(1);
    }
    if (hevc.sei) {
        std::fprintf(stderr, "fail: Cpu H.265 must keep sei off for now\n");
        std::exit(1);
    }
    if (sve::qp_map_block(hevc) != 32) {
        std::fprintf(stderr, "fail: qp_map_block helper\n");
        std::exit(1);
    }

    sve::EncoderConfig config;
    config.width = 1280;
    config.height = 720;
    config.fps = sve::Rational{25, 1};
    config.codec = sve::Codec::H264;

    const std::vector<sve::Rect> regions{sve::Rect{0, 16, 0, 16, -8}};
    sve::BackendFrameControl control;
    control.regions = regions;
    control.pts = 0;

    // Named, not called: opening a session needs the definition this header withholds.
    (void)&sve::IEncoderBackend::SubmitFrame;
    (void)&sve::IEncoderBackend::ReceivePacket;
    (void)&sve::IEncoderBackend::Flush;

    const sve::Status ok = sve::Status::Ok();
    (void)ok.ok();
    (void)sve::memory_type_supported(sve::MemoryType::CPU);
    (void)sve::memory_type_name(sve::MemoryType::ONEVPL);
}

[[maybe_unused]] void names_the_file_entry_point()
{
    std::atomic_bool cancel{false};
    const sve::Report r = sve::transcode_file(
        "in.mp4", "out.mp4", make_boxes(), make_settings(), &cancel);
    (void)r.encoder_used;
    (void)r.encoder_name;
    (void)r.frames;
    (void)r.regions_dropped;
    (void)r.cpu_h265_target_guard;
    (void)r.roi_lag_measured;
    (void)r.roi_lag_samples;
    (void)r.roi_lag_min_iou;
    (void)r.roi_lag_p95_frames;
    (void)r.roi_lag_p99_frames;
    (void)r.roi_lag_p95_ms;
    (void)r.roi_lag_p99_ms;
    (void)r.roi_lag_acceptance;
}

[[maybe_unused]] void names_the_frame_entry_point()
{
    sve::SessionParams p;
    p.width = 1280;
    p.height = 720;
    p.time_base_num = 1;
    p.time_base_den = 1000;
    p.codec = sve::InputCodec::H264;
    p.settings = make_settings();

    CountingHandler handler;
    sve::Session session(p, &handler);

    sve::Regions regions;
    const auto boxes = make_boxes();
    regions.boxes = boxes;

    // One access unit: the session above was opened with a codec, so this is the overload
    // that applies, and the compiler is the one enforcing that rather than a runtime check
    // on which field of a struct got filled.
    const std::vector<uint8_t> access_unit(64, 0);
    session.submit(access_unit, regions, 0);

    const sve::Report r = session.finish();
    (void)r.packets;
    (void)r.cpu_h265_target_guard;
    (void)r.roi_lag_measured;
    (void)r.roi_lag_samples;
    (void)r.roi_lag_min_iou;
    (void)r.roi_lag_p95_frames;
    (void)r.roi_lag_p99_frames;
    (void)r.roi_lag_p95_ms;
    (void)r.roi_lag_p99_ms;
    (void)r.roi_lag_acceptance;
}

} // namespace

int main()
{
    // The geometry is public for its own tests, and reachable from the same
    // include -- a caller building a mask wants it.
    std::vector<uint8_t> bytes(64 * 64, 0);
    sve::Mask mask;
    mask.data = bytes;
    mask.width = 64;
    mask.height = 64;
    mask.stride = 64;

    sve::MaskMapping mapping;
    const sve::QpDeltaMap grid = sve::qp_map_from_mask(mask, mapping, 64, 64);
    const std::vector<sve::Rect> rects = sve::rects_from_qp_map(grid, 32);
    if (grid.cols != 2 || grid.rows != 2 || rects.size() != 1) {
        std::fprintf(stderr, "public_api_test: unexpected geometry\n");
        return 1;
    }

    std::puts("public_api_test ok");
    return 0;
}
