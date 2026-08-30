// The encoder's lifecycle and its submit/receive contract -- TZ v3.4 §8.1 and §8.13.
//
// What is worth testing here is the state machine, not the picture: that a method called
// at the wrong moment answers rather than misbehaves, that backpressure and end-of-stream
// are codes rather than special cases, and that Stop can be called twice. The pixels
// themselves are covered by the integration suite, which has real clips to compare.
//
// Everything runs on the software backend, so this test needs no GPU.

#include "sve/encoder.h"
#include "sve/frame.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 96;

SmartEncoderConfig config_for(Encoder backend = Encoder::Cpu, Codec codec = Codec::H264)
{
    SmartEncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = backend;
    cfg.settings.codec = codec;
    cfg.settings.crf = 32;
    // Nothing here has a mask or boxes to embed, and libx264's SEI path is not what this
    // test is about.
    cfg.settings.metadata.bitstream_sei = false;
    return cfg;
}

/// Grey planes the encoder can take, held by a shared owner so the Frame keeps them alive.
struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

Frame make_frame(const std::shared_ptr<Pixels>& pixels, int64_t pts)
{
    RawFrameView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_format = PixelFormat::YUV420P;
    view.plane_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.timestamp = MediaTimestamp{pts, Rational{1, 25}};
    view.owner = pixels;

    Frame frame;
    CHECK(WrapRawFrame(view, &frame).ok());
    return frame;
}

std::unique_ptr<SmartVideoEncoder> open_encoder(const SmartEncoderConfig& cfg)
{
    auto made = SmartVideoEncoder::Create(cfg);
    CHECK(made.ok());
    return std::move(made.value());
}

void a_new_encoder_is_configured_and_knows_its_backend()
{
    auto enc = open_encoder(config_for());
    CHECK(enc->State() == EncoderState::Configured);
    const EncoderStatistics stats = enc->GetStatistics();
    CHECK(stats.frames_submitted == 0);
    CHECK(!stats.encoder_name.empty());

    // §8.11: a muxer can declare its stream before a single frame is submitted.
    CodecParameters params;
    CHECK(enc->GetCodecParameters(&params).ok());
    CHECK(params.width == kWidth);
    CHECK(params.height == kHeight);
    CHECK(params.codec == Codec::H264);
}

void an_input_format_that_cannot_exist_is_refused()
{
    SmartEncoderConfig cfg = config_for();
    cfg.input.height = 0;
    CHECK(SmartVideoEncoder::Create(cfg).status().code() == StatusCode::InvalidArgument);
}

void same_as_source_needs_a_source()
{
    // This entry point is handed frames, not a file, so there is nothing to mirror. Better
    // an InvalidArgument than an encoder that quietly picked one.
    SmartEncoderConfig cfg = config_for(Encoder::Cpu, Codec::SameAsSource);
    CHECK(SmartVideoEncoder::Create(cfg).status().code() == StatusCode::InvalidArgument);
}

void frames_go_in_and_packets_come_out()
{
    auto pixels = std::make_shared<Pixels>();
    auto enc = open_encoder(config_for());

    int64_t received = 0;
    for (int64_t i = 0; i < 10; ++i) {
        const Frame frame = make_frame(pixels, i);
        FrameMetadata meta;
        meta.timestamp = MediaTimestamp{i, Rational{1, 25}};
        CHECK(enc->SubmitFrame(frame.get(), meta).ok());
        CHECK(enc->State() == EncoderState::Running);

        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again) {
                break;
            }
            CHECK(s.ok());
            CHECK(!packet.payload.empty());
            CHECK(packet.codec == Codec::H264);
            CHECK(packet.time_base.den > 0);
            ++received;
        }
    }

    CHECK(enc->Flush().ok());
    CHECK(enc->State() == EncoderState::Draining);
    for (;;) {
        EncodedPacket packet;
        const Status s = enc->ReceivePacket(&packet);
        if (s.code() == StatusCode::EndOfStream) {
            break;
        }
        CHECK(s.ok());
        ++received;
    }
    CHECK(enc->State() == EncoderState::EndOfStream);
    CHECK(received == 10);

    const EncoderStatistics stats = enc->GetStatistics();
    CHECK(stats.frames_submitted == 10);
    CHECK(stats.packets_received == received);
    CHECK(stats.bytes_encoded > 0);
}

void submitting_after_flush_is_refused_rather_than_ignored()
{
    auto pixels = std::make_shared<Pixels>();
    auto enc = open_encoder(config_for());
    const Frame frame = make_frame(pixels, 0);
    FrameMetadata meta;
    CHECK(enc->SubmitFrame(frame.get(), meta).ok());
    CHECK(enc->Flush().ok());

    // §8.13: Draining, EndOfStream, Stopped and Failed all refuse a frame. Accepting one
    // and dropping it is the failure mode this replaces.
    CHECK(enc->SubmitFrame(frame.get(), meta).code() == StatusCode::InvalidArgument);
    CHECK(enc->Stop().ok());
    CHECK(enc->SubmitFrame(frame.get(), meta).code() == StatusCode::InvalidArgument);
}

void stop_is_idempotent()
{
    auto enc = open_encoder(config_for());
    CHECK(enc->Stop().ok());
    CHECK(enc->State() == EncoderState::Stopped);
    CHECK(enc->Stop().ok());
    CHECK(enc->State() == EncoderState::Stopped);
}

void a_null_frame_and_a_null_packet_are_arguments_not_crashes()
{
    auto enc = open_encoder(config_for());
    FrameMetadata meta;
    CHECK(enc->SubmitFrame(nullptr, meta).code() == StatusCode::InvalidArgument);
    CHECK(enc->ReceivePacket(nullptr).code() == StatusCode::InvalidArgument);
}

void a_keyframe_request_is_counted_even_though_it_is_best_effort()
{
    auto pixels = std::make_shared<Pixels>();
    auto enc = open_encoder(config_for());
    CHECK(enc->RequestKeyFrame().ok());

    const Frame frame = make_frame(pixels, 0);
    FrameMetadata meta;
    CHECK(enc->SubmitFrame(frame.get(), meta).ok());
    CHECK(enc->GetStatistics().keyframes_requested == 1);

    // The same asked for at submit time rather than before it.
    FrameMetadata forced;
    forced.force_key_frame = true;
    CHECK(enc->SubmitFrame(frame.get(), forced).ok());
    CHECK(enc->GetStatistics().keyframes_requested == 2);
}

void reconfigure_takes_what_8_2_allows_and_refuses_the_rest()
{
    auto enc = open_encoder(config_for());

    RuntimeEncoderSettings ok;
    ok.frame_rate = Rational{30, 1};
    ok.target_bitrate = 400000;
    CHECK(enc->Reconfigure(ok).ok());

    RuntimeEncoderSettings bad;
    bad.frame_rate = Rational{0, 1};
    CHECK(enc->Reconfigure(bad).code() == StatusCode::InvalidArgument);

    CHECK(enc->Stop().ok());
    CHECK(enc->Reconfigure(ok).code() == StatusCode::InvalidArgument);
}

void cpu_h265_refuses_a_target_bitrate_out_loud()
{
    // STATUS.md Р5. The guard is a standing contract, so it is asserted rather than left to
    // whoever next reads the log: at Create it downgrades and records why, and a caller who
    // asks again through Reconfigure is told instead of quietly ignored.
    SmartEncoderConfig cfg = config_for(Encoder::Cpu, Codec::H265);
    cfg.settings.rate_control.target_bitrate = 800000;
    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().cpu_h265_target_guard);

    RuntimeEncoderSettings again;
    again.target_bitrate = 800000;
    CHECK(enc->Reconfigure(again).code() == StatusCode::Unsupported);
}

void the_selection_orders_the_backends_and_can_exclude_them()
{
    // A named backend means it: the selection's permissions do not apply, because the
    // caller has already been explicit and quietly getting libx264 instead is the failure
    // this rule exists to prevent.
    BackendSelection none;
    none.preferred.clear();
    none.allow_software_fallback = false;
    CHECK(resolve_backends(Encoder::Cpu, none).size() == 1);

    // Auto walks the list, in the order given.
    BackendSelection cpu_first;
    cpu_first.preferred = {Encoder::Cpu, Encoder::Qsv};
    const auto chain = resolve_backends(Encoder::Auto, cpu_first);
    CHECK(chain.size() == 2);
    CHECK(chain[0].get().kind() == Encoder::Cpu);
    CHECK(chain[1].get().kind() == Encoder::Qsv);

    // Software can be shut out of the automatic path entirely.
    BackendSelection hardware_only;
    hardware_only.allow_software_fallback = false;
    for (const BackendRef& ref : resolve_backends(Encoder::Auto, hardware_only)) {
        CHECK(ref.get().kind() != Encoder::Cpu);
    }

    // And a backend absent from the list is not reached unless unlisted ones are allowed.
    BackendSelection only_nvenc;
    only_nvenc.preferred = {Encoder::Nvenc};
    CHECK(resolve_backends(Encoder::Auto, only_nvenc).size() == 1);
    only_nvenc.allow_unlisted = true;
    CHECK(resolve_backends(Encoder::Auto, only_nvenc).size() > 1);
}

void a_selection_that_excludes_everything_fails_to_open()
{
    SmartEncoderConfig cfg = config_for(Encoder::Auto);
    cfg.backend_selection.preferred.clear();
    cfg.backend_selection.allow_software_fallback = false;
    CHECK(!SmartVideoEncoder::Create(cfg).ok());
}

void diagnostics_go_where_the_caller_says()
{
    std::vector<std::pair<LogLevel, std::string>> lines;
    SmartEncoderConfig cfg = config_for(Encoder::Cpu, Codec::H265);
    cfg.settings.rate_control.target_bitrate = 800000;   // trips the CPU H.265 guard
    cfg.log = [&lines](LogLevel level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };

    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().cpu_h265_target_guard);

    // §50: the degradation reached the caller's sink, not somebody's terminal.
    bool guarded = false;
    for (const auto& [level, text] : lines) {
        if (level == LogLevel::Warning && text.find("cpu h265 target-bitrate") != std::string::npos) {
            guarded = true;
        }
    }
    CHECK(guarded);
    CHECK(enc->GetStatistics().fallbacks_applied == 1);

    // And which backend opened is said out loud rather than left to be inferred.
    bool announced = false;
    for (const auto& [level, text] : lines) {
        if (level == LogLevel::Info && text.find("backend:") != std::string::npos) {
            announced = true;
        }
    }
    CHECK(announced);
}

void strict_refuses_where_permissive_degrades()
{
    // Same request, two policies. Under Permissive the encoder opens and records that it
    // had to downgrade; under Strict it refuses, and the caller learns before encoding a
    // single frame rather than from a counter afterwards.
    SmartEncoderConfig permissive = config_for(Encoder::Cpu, Codec::H265);
    permissive.settings.rate_control.target_bitrate = 800000;
    permissive.log = [](LogLevel, std::string_view) {};
    CHECK(SmartVideoEncoder::Create(permissive).ok());

    SmartEncoderConfig strict = permissive;
    strict.settings.fallback = FallbackPolicy::Strict;
    const auto refused = SmartVideoEncoder::Create(strict);
    CHECK(!refused.ok());
    CHECK(refused.status().code() == StatusCode::Unsupported);
}

void strict_refuses_a_frame_whose_regions_do_not_fit()
{
    // The software backend has no region limit, so this asserts the shape rather than the
    // hardware's number: with nothing dropped, Strict changes nothing.
    auto pixels = std::make_shared<Pixels>();
    SmartEncoderConfig cfg = config_for();
    cfg.settings.fallback = FallbackPolicy::Strict;
    cfg.log = [](LogLevel, std::string_view) {};
    auto enc = open_encoder(cfg);

    const Frame frame = make_frame(pixels, 0);
    FrameMetadata meta;
    CHECK(enc->SubmitFrame(frame.get(), meta).ok());
    CHECK(enc->GetStatistics().fallbacks_applied == 0);
}

} // namespace

int main()
{
    a_new_encoder_is_configured_and_knows_its_backend();
    an_input_format_that_cannot_exist_is_refused();
    same_as_source_needs_a_source();
    frames_go_in_and_packets_come_out();
    submitting_after_flush_is_refused_rather_than_ignored();
    stop_is_idempotent();
    a_null_frame_and_a_null_packet_are_arguments_not_crashes();
    a_keyframe_request_is_counted_even_though_it_is_best_effort();
    reconfigure_takes_what_8_2_allows_and_refuses_the_rest();
    cpu_h265_refuses_a_target_bitrate_out_loud();
    the_selection_orders_the_backends_and_can_exclude_them();
    a_selection_that_excludes_everything_fails_to_open();
    diagnostics_go_where_the_caller_says();
    strict_refuses_where_permissive_degrades();
    strict_refuses_a_frame_whose_regions_do_not_fit();
    std::printf("ok\n");
    return 0;
}
