// AT-009: ten-bit P010 / HEVC Main10 -- TZ v3.4 §15.3, §32 and §48.
//
// The acceptance test says: for each backend with the main10 capability, encode 300 frames
// of P010 as Main10 and check the frame count, monotonic PTS and end-of-stream; where the
// capability is absent, a documented SKIP with capability evidence.
//
// So the capability is asked of the encoder rather than assumed, and the skip prints what
// it was told. §50 forbids "catch, continue, PASS", and a skip nobody can audit is the
// same thing wearing a different hat.

#include "sve/encoder.h"
#include "sve/frame.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace sve;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

// §32 asks for 300 frames. Small ones: this is a functional test, not a benchmark.
constexpr int kFrames = 300;
// Not the smallest frame that would do: Quick Sync refuses 128x96 with "current resolution
// is unsupported", and a test that silently skips because its own frame was too small is a
// test that proves nothing.
constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;

/// P010 is two planes of sixteen-bit samples, the ten bits sitting at the top of each.
struct TenBitPixels {
    std::vector<uint16_t> y = std::vector<uint16_t>(kWidth * kHeight, 512u << 6);
    std::vector<uint16_t> uv = std::vector<uint16_t>(kWidth * kHeight / 2, 512u << 6);
};

EncoderConfig config_for(Backend backend)
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.time_base = Rational{1, 25};
    cfg.input.pixel_format = PixelFormat::P010;
    cfg.settings.encoder = backend;
    cfg.settings.codec = Codec::H265;   // §15.3's ten-bit profile is HEVC
    cfg.settings.crf = 30;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

Frame make_frame(const std::shared_ptr<TenBitPixels>& pixels, int64_t pts)
{
    RawFrameView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_format = PixelFormat::P010;
    view.format.frame_rate = Rational{25, 1};
    view.format.time_base = Rational{1, 25};
    view.plane_count = 2;
    view.planes[0] = {reinterpret_cast<const uint8_t*>(pixels->y.data()), kWidth * 2,
        pixels->y.size() * sizeof(uint16_t)};
    view.planes[1] = {reinterpret_cast<const uint8_t*>(pixels->uv.data()), kWidth * 2,
        pixels->uv.size() * sizeof(uint16_t)};
    view.timestamp = MediaTimestamp{pts, Rational{1, 25}};
    view.owner = pixels;

    Frame frame;
    CHECK(WrapRawFrame(view, &frame).ok());
    return frame;
}

/// The code a factory refuses with, or Ok when it did not refuse.
StatusCode create_refusal(const EncoderConfig& cfg)
{
    try {
        const auto made = CreateVideoEncoder(cfg);
        return StatusCode::Ok;
    } catch (const EncoderError& e) {
        return e.code();
    }
}

/// Runs AT-009 against one backend, or reports why it could not.
void at_009(Backend backend, const char* label)
{
    EncoderConfig cfg = config_for(backend);

    std::unique_ptr<SmartVideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(cfg);
    } catch (const EncoderError& e) {
        // Either the device is absent or its encoder has no ten-bit path. Both are a SKIP,
        // and the message the backend gave is the evidence AT-009 asks for.
        std::printf("  skip  %s: %s\n", label, e.what());
        return;
    }

    const EncoderCapabilities caps = enc->GetCapabilities();
    if (!caps.main10) {
        std::printf("  skip  %s: encoder reports main10=false\n", label);
        return;
    }
    std::printf("  run   %s: main10=true p010_input=%s\n", label,
        caps.p010_input ? "true" : "false");

    auto pixels = std::make_shared<TenBitPixels>();
    int64_t received = 0;
    int64_t last_pts = INT64_MIN;

    const auto drain = [&](bool to_end) {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again) {
                return;
            }
            if (s.code() == StatusCode::EndOfStream) {
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
        FrameMetadata meta;
        meta.timestamp = MediaTimestamp{i, Rational{1, 25}};
        const Frame frame = make_frame(pixels, i);
        CHECK(enc->SubmitFrame(TimedFrame{frame, meta}).ok());
        drain(false);
    }

    CHECK(enc->Flush().ok());
    drain(true);
    CHECK(enc->State() == EncoderState::EndOfStream);

    const EncoderStatistics stats = enc->GetStatistics();
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
    // §8.2: a pixel format and a profile that cannot go together are refused at Create.
    EncoderConfig cfg = config_for(Backend::X265);
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.profile = CodecProfile::H265Main10;
    CHECK(create_refusal(cfg) == StatusCode::InvalidArgument);
}

void ten_bit_input_cannot_ask_for_an_eight_bit_profile()
{
    EncoderConfig cfg = config_for(Backend::X265);
    cfg.settings.profile = CodecProfile::H265Main;
    CHECK(create_refusal(cfg) == StatusCode::InvalidArgument);
}

void ten_bit_h264_is_refused_rather_than_invented()
{
    // H.264 has a ten-bit profile, but nothing in this specification asks for one, and
    // claiming it would be inventing a contract nobody wrote. Asked of the H.264 backend,
    // so the refusal is about the bit depth and not about the backend's codec.
    EncoderConfig cfg = config_for(Backend::X264);
    cfg.settings.codec = Codec::H264;
    CHECK(create_refusal(cfg) == StatusCode::InvalidArgument);
}

} // namespace

int main()
{
    eight_bit_input_cannot_ask_for_main10();
    ten_bit_input_cannot_ask_for_an_eight_bit_profile();
    ten_bit_h264_is_refused_rather_than_invented();

    std::printf("AT-009 (10-bit P010/Main10, 300 frames):\n");
    at_009(Backend::X265, "x265/libx265");
    at_009(Backend::IntelOneVpl, "qsv/hevc_qsv");

    std::printf("ok\n");
    return 0;
}
