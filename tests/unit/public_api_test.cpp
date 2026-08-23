// A consumer's view: one include, one namespace, both entry points. If this
// stops compiling, the public surface has grown a dependency it should not have.
//
// It is a compile test with a runtime tail: transcode_file and Session are only
// named here, not driven, because exercising them needs media. What it proves is
// the shape -- that "smartvideo/smartvideo.h" alone is enough to write against.

#include "smartvideo/smartvideo.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Everything a caller needs to say, said with the public vocabulary only.
smartvideo::Settings make_settings()
{
    smartvideo::Settings s;
    s.encoder = smartvideo::Encoder::Auto;
    s.crf = 28;
    s.gop = 1;
    s.preset = smartvideo::Preset::Balanced;
    s.quality.background_qp_delta = 20;
    s.quality.hold_sec = 0.5;
    s.quality.max_regions = 32;
    s.quality.kinds["plate"] = smartvideo::KindQuality{-8, 0.01};
    s.quality.mask_qp_delta_at_255 = -8;
    s.quality.mask_levels = 2;
    return s;
}

std::vector<smartvideo::Box> make_boxes()
{
    return {smartvideo::Box{
        .t_sec = 1.25,
        .kind = "plate",
        .x = 0.35f,
        .y = 0.40f,
        .width = 0.30f,
        .height = 0.20f,
    }};
}

// Receiving packets is part of the surface too -- and it is FFmpeg-free, which
// is why this file compiles without libav* anywhere on its include path.
class CountingHandler : public smartvideo::PacketHandler {
public:
    void on_open(const smartvideo::StreamInfo& info) override
    {
        width = info.width;
        name = info.encoder_name;
    }
    void on_packet(const smartvideo::EncodedPacket& p) override
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
    for (const smartvideo::IEncoderBackend& b :
        smartvideo::backend_chain(smartvideo::Encoder::Cpu)) {
        const smartvideo::EncoderCapabilities named = b.GetCapabilities(
            smartvideo::Codec::H264);
        (void)named.h264;
    }
    const smartvideo::EncoderCapabilities caps =
        smartvideo::backend_chain(smartvideo::Encoder::Cpu)[0].get().GetCapabilities(
            smartvideo::Codec::H264);
    (void)caps.h264;
    (void)caps.per_block_qp;
    (void)caps.max_roi_regions;

    smartvideo::EncoderConfig config;
    config.width = 1280;
    config.height = 720;
    config.fps = smartvideo::Rational{25, 1};
    config.codec = smartvideo::Codec::H264;

    const std::vector<smartvideo::Rect> regions{smartvideo::Rect{0, 16, 0, 16, -8}};
    smartvideo::BackendFrameControl control;
    control.regions = regions;
    control.pts = 0;

    // Named, not called: opening a session needs the definition this header withholds.
    (void)&smartvideo::IEncoderBackend::SubmitFrame;
    (void)&smartvideo::IEncoderBackend::ReceivePacket;
    (void)&smartvideo::IEncoderBackend::Flush;

    const smartvideo::Status ok = smartvideo::Status::Ok();
    (void)ok.ok();
    (void)smartvideo::memory_type_supported(smartvideo::MemoryType::CPU);
    (void)smartvideo::memory_type_name(smartvideo::MemoryType::ONEVPL);
}

[[maybe_unused]] void names_the_file_entry_point()
{
    std::atomic_bool cancel{false};
    const smartvideo::Report r = smartvideo::transcode_file(
        "in.mp4", "out.mp4", make_boxes(), make_settings(), &cancel);
    (void)r.encoder_used;
    (void)r.encoder_name;
    (void)r.frames;
    (void)r.regions_dropped;
}

[[maybe_unused]] void names_the_frame_entry_point()
{
    smartvideo::SessionParams p;
    p.width = 1280;
    p.height = 720;
    p.time_base_num = 1;
    p.time_base_den = 1000;
    p.codec = smartvideo::InputCodec::H264;
    p.settings = make_settings();

    CountingHandler handler;
    smartvideo::Session session(p, &handler);

    smartvideo::Regions regions;
    const auto boxes = make_boxes();
    regions.boxes = boxes;

    // One access unit: the session above was opened with a codec, so this is the overload
    // that applies, and the compiler is the one enforcing that rather than a runtime check
    // on which field of a struct got filled.
    const std::vector<uint8_t> access_unit(64, 0);
    session.submit(access_unit, regions, 0);

    const smartvideo::Report r = session.finish();
    (void)r.packets;
}

} // namespace

int main()
{
    // The geometry is public for its own tests, and reachable from the same
    // include -- a caller building a mask wants it.
    std::vector<uint8_t> bytes(64 * 64, 0);
    smartvideo::Mask mask;
    mask.data = bytes;
    mask.width = 64;
    mask.height = 64;
    mask.stride = 64;

    smartvideo::MaskMapping mapping;
    const smartvideo::QpDeltaMap grid = smartvideo::qp_map_from_mask(mask, mapping, 64, 64);
    const std::vector<smartvideo::Rect> rects = smartvideo::rects_from_qp_map(grid, 32);
    if (grid.cols != 2 || grid.rows != 2 || rects.size() != 1) {
        std::fprintf(stderr, "public_api_test: unexpected geometry\n");
        return 1;
    }

    std::puts("public_api_test ok");
    return 0;
}
