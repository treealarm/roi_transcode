// Ten-bit P010 / HEVC Main10.
//
// For each backend with the main10 capability, encode 300 frames of P010 as Main10 and
// check the frame count, monotonic PTS and end-of-stream; where the capability is absent,
// a SKIP with the capability evidence.
//
// So the capability is asked of the encoder rather than assumed, and the skip prints what
// it was told: "catch, continue, PASS" with a skip nobody can audit would be a silent pass
// wearing a different hat.

#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ta_roienc;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

// 300 frames, small ones: this is a functional test, not a benchmark.
constexpr int kFrames = 300;
// Not the smallest frame that would do: Quick Sync refuses 128x96 with "current resolution
// is unsupported", and a test that silently skips because its own frame was too small is a
// test that proves nothing.
constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;

/// P010 is two planes of sixteen-bit samples, the ten bits sitting at the top of each.
struct TenBitSamples {
    std::vector<uint16_t> y = std::vector<uint16_t>(kWidth * kHeight, 512u << 6);
    std::vector<uint16_t> uv = std::vector<uint16_t>(kWidth * kHeight / 2, 512u << 6);
};

EncodeOptions config_for(Engine backend)
{
    EncodeOptions cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Ratio{25, 1};
    cfg.input.time_base = Ratio{1, 25};
    cfg.input.pixel_layout = PixelLayout::P010;
    cfg.settings.encoder = backend;
    cfg.settings.codec = Coding::H265;   // the ten-bit profile here is HEVC's
    cfg.settings.crf = 30;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](Verbosity, std::string_view) {};
    return cfg;
}

Picture make_frame(const std::shared_ptr<TenBitSamples>& pixels, int64_t pts)
{
    PictureView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_layout = PixelLayout::P010;
    view.format.frame_rate = Ratio{25, 1};
    view.format.time_base = Ratio{1, 25};
    view.layer_count = 2;
    view.planes[0] = {reinterpret_cast<const uint8_t*>(pixels->y.data()), kWidth * 2,
        pixels->y.size() * sizeof(uint16_t)};
    view.planes[1] = {reinterpret_cast<const uint8_t*>(pixels->uv.data()), kWidth * 2,
        pixels->uv.size() * sizeof(uint16_t)};
    view.timestamp = Stamp{pts, Ratio{1, 25}};
    view.owner = pixels;

    Picture frame;
    CHECK(WrapPicture(view, &frame).ok());
    return frame;
}

/// The code a factory refuses with, or Ok when it did not refuse.
VerdictCode create_refusal(const EncodeOptions& cfg)
{
    try {
        const auto made = CreateVideoEncoder(cfg);
        return VerdictCode::Ok;
    } catch (const EncodeError& e) {
        return e.code();
    }
}

/// Runs the ten-bit encode against one backend, or reports why it could not.
void ten_bit_encodes(Engine backend, const char* label)
{
    EncodeOptions cfg = config_for(backend);

    std::unique_ptr<VideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(cfg);
    } catch (const EncodeError& e) {
        // Either the device is absent or its encoder has no ten-bit path. Both are a SKIP,
        // and the message the backend gave is the evidence.
        std::printf("  skip  %s: %s\n", label, e.what());
        return;
    }

    const EngineAbilities caps = enc->GetCapabilities();
    if (!caps.main10) {
        std::printf("  skip  %s: encoder reports main10=false\n", label);
        return;
    }
    std::printf("  run   %s: main10=true p010_input=%s\n", label,
        caps.p010_input ? "true" : "false");

    auto pixels = std::make_shared<TenBitSamples>();
    int64_t received = 0;
    int64_t last_pts = INT64_MIN;

    const auto drain = [&](bool to_end) {
        for (;;) {
            AccessUnit packet;
            const Verdict s = enc->ReceiveAccessUnit(&packet);
            if (s.code() == VerdictCode::Again) {
                return;
            }
            if (s.code() == VerdictCode::EndOfStream) {
                CHECK(to_end);
                return;
            }
            CHECK(s.ok());
            CHECK(!packet.payload.empty());
            // Monotonic, and strictly so: two packets sharing a decode timestamp is what
            // the mp4 muxer refuses, and it is worth catching here rather than there.
            CHECK(packet.dts == kNoTimestamp || packet.dts > last_pts);
            if (packet.dts != kNoTimestamp) {
                last_pts = packet.dts;
            }
            ++received;
        }
    };

    for (int64_t i = 0; i < kFrames; ++i) {
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        const Picture frame = make_frame(pixels, i);
        CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).ok());
        drain(false);
    }

    CHECK(enc->Flush().ok());
    drain(true);
    CHECK(enc->Phase() == Lifecycle::EndOfStream);

    const Counters stats = enc->GetStatistics();
    CHECK(stats.frames_submitted == kFrames);
    CHECK(received == kFrames);
    CHECK(stats.bytes_encoded > 0);
    std::printf("        %lld frames in, %lld packets out, %lld bytes\n",
        static_cast<long long>(stats.frames_submitted),
        static_cast<long long>(received),
        static_cast<long long>(stats.bytes_encoded));
}

void eight_bit_input_cannot_ask_for_main10()
{
    // A pixel format and a profile that cannot go together are refused at Create.
    EncodeOptions cfg = config_for(Engine::X265);
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.profile = CodingProfile::H265Main10;
    CHECK(create_refusal(cfg) == VerdictCode::InvalidArgument);
}

void ten_bit_input_cannot_ask_for_an_eight_bit_profile()
{
    EncodeOptions cfg = config_for(Engine::X265);
    cfg.settings.profile = CodingProfile::H265Main;
    CHECK(create_refusal(cfg) == VerdictCode::InvalidArgument);
}

void ten_bit_h264_is_refused_rather_than_invented()
{
    // H.264 has a ten-bit profile, but nothing here supports it, and claiming it would be
    // inventing a contract nobody wrote. Asked of the H.264 backend, so the refusal is
    // about the bit depth and not about the backend's codec.
    EncodeOptions cfg = config_for(Engine::X264);
    cfg.settings.codec = Coding::H264;
    CHECK(create_refusal(cfg) == VerdictCode::InvalidArgument);
}

} // namespace

int main()
{
    eight_bit_input_cannot_ask_for_main10();
    ten_bit_input_cannot_ask_for_an_eight_bit_profile();
    ten_bit_h264_is_refused_rather_than_invented();

    std::printf("10-bit P010/Main10, 300 frames:\n");
    ten_bit_encodes(Engine::X265, "x265/libx265");
    ten_bit_encodes(Engine::IntelOneVpl, "qsv/hevc_qsv");

    std::printf("ok\n");
    return 0;
}
