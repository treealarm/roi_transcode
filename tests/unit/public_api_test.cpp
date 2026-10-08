// A consumer's view: one include, one namespace, both entry points. If this
// stops compiling, the public surface has grown a dependency it should not have.
//
// It is a compile test with a runtime tail: transcode_file and Session are only
// named here, not driven, because exercising them needs media. What it proves is
// the shape -- that "ta_roienc/ta_roienc.h" alone is enough to write against.

#include "ta_roienc/ta_roienc.h"

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Everything a caller needs to say, said with the public vocabulary only.
ta_roienc::Settings make_settings()
{
    ta_roienc::Settings s;
    s.encoder = ta_roienc::Backend::Auto;
    s.crf = 28;
    s.gop = 1;
    s.preset = ta_roienc::Preset::Balanced;
    s.quality.background_qp_delta = 20;
    s.quality.hold_sec = 0.5;
    s.quality.max_regions = 32;
    s.quality.kinds["plate"] = ta_roienc::KindQuality{-8, 0.01};
    s.quality.mask_qp_delta_at_255 = -8;
    s.quality.mask_levels = 2;
    return s;
}

std::vector<ta_roienc::Box> make_boxes()
{
    return {ta_roienc::Box{
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
class CountingHandler : public ta_roienc::PacketHandler {
public:
    void on_open(const ta_roienc::StreamInfo& info) override
    {
        width = info.width;
        name = info.backend_name;
    }
    void on_packet(const ta_roienc::EncodedPacket& p) override
    {
        ++packets;
        bytes += p.payload.size();
    }
    void on_end() override { ++ends; }

    int width = 0;
    std::string name;
    int packets = 0;
    std::size_t bytes = 0;
    int ends = 0;
};

// The backend interface, named with the public vocabulary alone. This is the part that keeps
// the interface honest: BackendSession is opaque here, so an AVCodecContext creeping into
// the public header would stop this file compiling -- which is the point of compiling it
// without libav* on the include path at all.
[[maybe_unused]] void names_the_backend_interface()
{
    for (const ta_roienc::BackendRef& ref :
        ta_roienc::resolve_backends(ta_roienc::Backend::X264, {}, ta_roienc::Codec::H264)) {
        const ta_roienc::IEncoderBackend& b = ref.get();
        const ta_roienc::EncoderCapabilities named = b.GetCapabilities(
            ta_roienc::Codec::H264);
        (void)named.h264;
    }
    const ta_roienc::EncoderCapabilities caps =
        ta_roienc::resolve_backends(ta_roienc::Backend::X264, {}, ta_roienc::Codec::H264)[0]
            .get().GetCapabilities(ta_roienc::Codec::H264);
    (void)caps.h264;
    (void)caps.qp_map;
    (void)caps.qp_map_block_width;
    (void)caps.qp_map_block_height;
    (void)caps.sei;
    (void)caps.native_bitrate;
    (void)caps.max_roi_regions;
    if (!caps.qp_map || caps.qp_map_block_width != 16 || caps.qp_map_block_height != 16) {
        std::fprintf(stderr, "fail: x264 must advertise qp_map 16x16\n");
        std::exit(1);
    }
    if (!caps.sei) {
        std::fprintf(stderr, "fail: x264 must advertise sei\n");
        std::exit(1);
    }
    if (!caps.native_bitrate) {
        std::fprintf(stderr, "fail: x264 must advertise native_bitrate\n");
        std::exit(1);
    }
    // The HEVC answer comes from the other backend, and asking x264 for it gives an empty
    // capability rather than a smaller one -- which is what selection reads.
    if (caps.h265) {
        std::fprintf(stderr, "fail: x264 must not advertise H.265\n");
        std::exit(1);
    }
    const ta_roienc::EncoderCapabilities hevc =
        ta_roienc::resolve_backends(ta_roienc::Backend::X265, {}, ta_roienc::Codec::H265)[0]
            .get().GetCapabilities(ta_roienc::Codec::H265);
    if (!hevc.qp_map || hevc.qp_map_block_width != 32) {
        std::fprintf(stderr, "fail: x265 must advertise qp_map 32x32\n");
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
    if (ta_roienc::qp_map_block(hevc) != 32) {
        std::fprintf(stderr, "fail: qp_map_block helper\n");
        std::exit(1);
    }

    ta_roienc::BackendConfig config;
    config.input.width = 1280;
    config.input.height = 720;
    config.input.frame_rate = ta_roienc::Rational{25, 1};
    config.codec = ta_roienc::Codec::H264;

    const std::vector<ta_roienc::Rect> regions{ta_roienc::Rect{0, 16, 0, 16, -8}};
    ta_roienc::BackendFrameControl control;
    control.regions = regions;
    control.pts = 0;

    // Named, not called: opening a session needs the definition this header withholds.
    (void)&ta_roienc::IEncoderBackend::SubmitFrame;
    (void)&ta_roienc::IEncoderBackend::ReceivePacket;
    (void)&ta_roienc::IEncoderBackend::Flush;

    const ta_roienc::Status ok = ta_roienc::Status::Ok();
    (void)ok.ok();
    (void)ta_roienc::memory_type_supported(ta_roienc::MemoryType::CPU);
    (void)ta_roienc::memory_type_name(ta_roienc::MemoryType::ONEVPL);
}

[[maybe_unused]] void names_the_file_entry_point()
{
    std::atomic_bool cancel{false};
    const ta_roienc::Report r = ta_roienc::transcode_file(
        "in.mp4", "out.mp4", make_boxes(), make_settings(), &cancel);
    (void)r.encoder_used;
    (void)r.backend_name;
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
    ta_roienc::SessionParams p;
    p.width = 1280;
    p.height = 720;
    p.time_base_num = 1;
    p.time_base_den = 1000;
    p.codec = ta_roienc::InputCodec::H264;
    p.settings = make_settings();

    CountingHandler handler;
    ta_roienc::Session session(p, &handler);

    ta_roienc::Regions regions;
    const auto boxes = make_boxes();
    regions.boxes = boxes;

    // One access unit: the session above was opened with a codec, so this is the overload
    // that applies, and the compiler is the one enforcing that rather than a runtime check
    // on which field of a struct got filled.
    const std::vector<uint8_t> access_unit(64, 0);
    session.submit(access_unit, regions, 0);

    const ta_roienc::Report r = session.finish();
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
    ta_roienc::Mask mask;
    mask.data = bytes;
    mask.width = 64;
    mask.height = 64;
    mask.stride = 64;

    ta_roienc::MaskMapping mapping;
    const ta_roienc::QpDeltaMap grid = ta_roienc::qp_map_from_mask(mask, mapping, 64, 64);
    const std::vector<ta_roienc::Rect> rects = ta_roienc::rects_from_qp_map(grid, 32);
    if (grid.cols != 2 || grid.rows != 2 || rects.size() != 1) {
        std::fprintf(stderr, "public_api_test: unexpected geometry\n");
        return 1;
    }

    // Every backend has a name that gets back to it. The table of names is what both the
    // parser and every help text now read, so the failure this guards against is an
    // enumerator added without a spelling -- which is how the list went stale before.
    for (const ta_roienc::Backend kind : {ta_roienc::Backend::IntelOneVpl,
             ta_roienc::Backend::Vaapi, ta_roienc::Backend::X264, ta_roienc::Backend::X265,
             ta_roienc::Backend::X264Direct, ta_roienc::Backend::X265Direct,
             ta_roienc::Backend::IntelOneVplDirect}) {
        if (ta_roienc::backend_choice_from_name(ta_roienc::backend_name(kind)).kind != kind) {
            std::fprintf(stderr, "public_api_test: %s does not round-trip through its name\n",
                ta_roienc::backend_name(kind));
            return 1;
        }
    }

    // And the refusal names them all, so a typo is answered with the full list of what
    // does exist.
    const std::string names = ta_roienc::backend_choice_names();
    try {
        ta_roienc::backend_choice_from_name("no-such-encoder");
        std::fprintf(stderr, "public_api_test: an unknown backend name was accepted\n");
        return 1;
    } catch (const std::invalid_argument& ex) {
        const std::string_view what = ex.what();
        for (const char* name : {"x264direct", "x265direct", "onevpldirect", "cpu", "auto"}) {
            if (names.find(name) == std::string::npos
                || what.find(name) == std::string_view::npos) {
                std::fprintf(stderr, "public_api_test: %s is missing from '%s'\n",
                    name, ex.what());
                return 1;
            }
        }
    }

    std::puts("public_api_test ok");
    return 0;
}
