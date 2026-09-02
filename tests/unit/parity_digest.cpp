// One half of the C++/Python equivalence check -- TZ v4.2 §58.10.1.
//
// §58.10 accepts the Python wrapper only if "основные C++ и Python сценарии дают
// функционально эквивалентные packets, PTS, metadata и capability reports при одинаковой
// конфигурации". Until 2026-09-02 nothing compared the two: the Python suite checked that
// Python worked and the C++ suite checked that C++ worked, and a binding that quietly
// encoded something else would have passed both. It had already drifted in five places by
// the time anyone looked (docs/STATUS.md, slice Z8), which is what a claim nobody checks
// tends to do.
//
// This prints a digest of one fixed encode. integration/scripts/python-parity.py prints the
// same digest through the wrapper, and check.sh diffs them. A digest rather than a hash: when
// it differs, the line that differs says whether the streams parted at the first packet or
// the last, and a hash would say only "not equal".
//
// **The frames are the load-bearing part.** Both sides must produce byte-identical input or
// the comparison measures the generator instead of the binding. The pattern below is integer
// arithmetic with no floating point and no library call, so NumPy reproduces it exactly; if
// this ever gains a multiply-by-0.5, the two sides stop being comparable and the test starts
// lying.

#include "sve/encoder.h"
#include "sve/raw_frame.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace sve;

namespace {

constexpr int32_t kWidth = 160;
constexpr int32_t kHeight = 120;
constexpr int32_t kFps = 25;
constexpr int64_t kFrames = 25;
constexpr int32_t kCrf = 30;
/// One keyframe interval inside the run, so the digest covers a GOP boundary rather than a
/// single all-intra stretch.
constexpr int32_t kGop = 10;

/// A moving checkerboard. Integer arithmetic only -- see the note at the top of the file.
struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);

    explicit Pixels(int64_t n)
    {
        for (int32_t row = 0; row < kHeight; ++row) {
            for (int32_t col = 0; col < kWidth; ++col) {
                const bool lit = ((col + static_cast<int32_t>(n) * 3) / 8 + row / 8) % 2 == 0;
                y[static_cast<std::size_t>(row) * kWidth + col] = lit ? 220 : 40;
            }
        }
    }
};

/// FNV-1a over every plane of every frame, so that "the two sides encoded different
/// pixels" and "the two sides encoded the same pixels differently" are distinguishable in
/// the digest rather than by guesswork. The first version of this file had no such line and
/// the first run differed; without this there was nothing to say which of the two it was.
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void fold(uint64_t& h, const std::vector<uint8_t>& bytes)
{
    for (const uint8_t b : bytes) {
        h ^= b;
        h *= kFnvPrime;
    }
}

} // namespace

int main()
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{kFps, 1};
    // Set explicitly, and this is why: leaving it to the default (1/90000) while the Python
    // side set 1/25 made the first run of this comparison differ by 9% in bytes with the
    // input hash identical and the ROI decision identical. Every field either side sets has
    // to be set by both, or the digest measures the test rather than the binding.
    cfg.input.time_base = Rational{1, kFps};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = Backend::X264Direct;
    cfg.settings.backend_selection.preferred = {Backend::X264Direct};
    cfg.settings.codec = Codec::H264;
    cfg.settings.crf = kCrf;
    cfg.settings.gop = kGop;
    // Off, and deliberately: the SEI payload carries no timestamp of its own but it does
    // carry what the two sides are being compared on, and a difference in it would show up
    // as a difference in packet size with nothing to say which field moved. §19.3's sidecar
    // is where metadata correspondence is checked, on both sides, by its own step.
    cfg.settings.metadata.bitstream_sei = false;
    // Diagnostics would land in the digest. The Python side silences its own the same way.
    cfg.log = [](LogLevel, std::string_view) {};

    std::unique_ptr<SmartVideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parity_digest: cannot open x264direct: %s\n", e.what());
        return 2;
    }

    const EncoderCapabilities caps = enc->GetCapabilities();
    std::printf("capability h264=%d h265=%d roi_rect=%d qp_map=%d block=%ux%u sei=%d "
                "native_bitrate=%d main10=%d p010=%d max_regions=%u\n",
        caps.h264, caps.h265, caps.roi_rectangles, caps.qp_map, caps.qp_map_block_width,
        caps.qp_map_block_height, caps.sei, caps.native_bitrate, caps.main10,
        caps.p010_input, caps.max_roi_regions);

    uint64_t input_hash = kFnvOffsetBasis;
    std::vector<EncodedPacket> packets;
    const auto drain = [&] {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return true;
            }
            if (!s.ok()) {
                std::fprintf(stderr, "parity_digest: receive: %s\n", s.message().c_str());
                return false;
            }
            packets.push_back(std::move(packet));
        }
    };

    for (int64_t i = 0; i < kFrames; ++i) {
        auto pixels = std::make_shared<Pixels>(i);
        fold(input_hash, pixels->y);
        fold(input_hash, pixels->u);
        fold(input_hash, pixels->v);
        RawFrameView view;
        view.format = cfg.input;
        view.plane_count = 3;
        view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
        view.timestamp = MediaTimestamp{i, Rational{1, kFps}};
        view.owner = pixels;

        TimedFrame timed;
        if (const Status s = WrapRawFrame(view, &timed.frame); !s.ok()) {
            std::fprintf(stderr, "parity_digest: wrap: %s\n", s.message().c_str());
            return 2;
        }
        timed.metadata.timestamp = view.timestamp;

        // One protected object, so the digest covers the ROI path rather than a plain
        // encode: the boxes are what the two languages disagree about if they disagree.
        Box box;
        box.kind = "plate";
        box.confidence = 0.9f;
        box.x = 0.35f;
        box.y = 0.40f;
        box.width = 0.30f;
        box.height = 0.20f;
        const std::vector<Box> boxes{box};
        timed.metadata.regions.boxes = boxes;

        if (const Status s = enc->SubmitFrame(timed); !s.ok()) {
            std::fprintf(stderr, "parity_digest: submit %lld: %s\n",
                static_cast<long long>(i), s.message().c_str());
            return 2;
        }
        if (!drain()) {
            return 2;
        }
    }
    if (const Status s = enc->Flush(); !s.ok()) {
        std::fprintf(stderr, "parity_digest: flush: %s\n", s.message().c_str());
        return 2;
    }
    if (!drain()) {
        return 2;
    }

    std::printf("input %016llx\n", static_cast<unsigned long long>(input_hash));
    std::printf("packets %zu\n", packets.size());
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const EncodedPacket& p = packets[i];
        std::printf("packet %zu pts=%lld dts=%lld key=%d bytes=%zu\n", i,
            static_cast<long long>(p.pts), static_cast<long long>(p.dts), p.key_frame,
            p.payload.size());
    }

    const EncoderStatistics stats = enc->GetStatistics();
    std::printf("stats frames=%lld packets=%lld bytes=%lld converted=%lld uploaded=%lld "
                "regions_dropped=%lld fallbacks=%lld clamped=%lld guard=%d\n",
        static_cast<long long>(stats.frames_submitted),
        static_cast<long long>(stats.packets_received),
        static_cast<long long>(stats.bytes_encoded),
        static_cast<long long>(stats.frames_converted),
        static_cast<long long>(stats.frames_uploaded),
        static_cast<long long>(stats.regions_dropped),
        static_cast<long long>(stats.fallbacks_applied),
        static_cast<long long>(stats.frames_quantiser_clamped),
        stats.cpu_h265_target_guard);
    return 0;
}
