// The facade when the backend misbehaves -- TZ v4.2 §8.13, §15, §19.4 and §50.
//
// encoder_test drives SmartVideoEncoder through libx264, which works: it opens, it takes
// frames, it hands packets back, and it has rate control of its own. That leaves the other
// half of smart_video_encoder.cpp unexecuted -- the exception that has to become a Status,
// the Failed state every entry point must then report, the outer bitrate loop of §15 that
// only runs for a backend without its own, and the §50 counters that only move when a
// backend says something went wrong.
//
// Reached through CreateVideoEncoderIn (src/core/encoder_assembly.h) with a registry holding
// nothing but the fake. Not a switch inside the shipping encoder: a fault path that exists
// only because it is tested is worse than an untested one, because it also ships.

#include "encoder_assembly.h"

#include "fake_backend.h"
#include "sve/encoder.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace sve;
using sve::testing::FakeBackend;
using sve::testing::FakeBuffer;
using sve::testing::registry_with;

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

EncoderConfig config_for(const FakeBackend& backend)
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{FakeBackend::kFrameRate, 1};
    cfg.input.pixel_format = backend.pixels;
    cfg.settings.encoder = backend.kind();
    cfg.settings.codec = backend.serves;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

FakeBackend backend_for_facade()
{
    FakeBackend backend;
    backend.width = kWidth;
    backend.height = kHeight;
    // The format the facade will hand over untouched, so that nothing here is measuring a
    // pixel conversion by accident.
    backend.pixels = PixelFormat::YUV420P;
    return backend;
}

/// Submits one frame through the borrowing overload, which is the shortest way in that
/// needs no pixel plumbing of its own.
Status submit_one(SmartVideoEncoder& encoder, FakeBuffer& pixels, int64_t index)
{
    FrameMetadata meta;
    meta.timestamp = MediaTimestamp{index * FakeBackend::kTicksPerFrame,
        Rational{1, FakeBackend::kTimeBaseDen}};
    return encoder.SubmitFrame(pixels, meta);
}

void a_backend_that_throws_becomes_a_status_not_an_exception()
{
    FakeBackend backend = backend_for_facade();
    backend.submit_throws = true;
    const BackendRegistry registry = registry_with(backend);
    auto enc = CreateVideoEncoderIn(config_for(backend), registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);

    // §8.13: the exception stops here. A backend on its own SDK is C++ underneath, and a
    // throw travelling out of SubmitFrame would cross an API that returns Status.
    const Status s = submit_one(*enc, pixels, 0);
    CHECK(!s.ok());
    CHECK(s.code() == StatusCode::BackendError);
    CHECK(s.message().find("threw on a frame") != std::string::npos);
    CHECK(enc->State() == EncoderState::Failed);
}

void every_entry_point_reports_the_failure_that_happened()
{
    FakeBackend backend = backend_for_facade();
    backend.submit_throws = true;
    const BackendRegistry registry = registry_with(backend);
    auto enc = CreateVideoEncoderIn(config_for(backend), registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);
    CHECK(!submit_one(*enc, pixels, 0).ok());

    // Once Failed, everything answers with the failure that caused it rather than with a
    // fresh complaint about the state -- the caller needs to know what broke, and only the
    // first answer knows.
    const std::string cause = "threw on a frame";
    EncodedPacket packet;
    CHECK(enc->ReceivePacket(&packet).message().find(cause) != std::string::npos);
    CHECK(enc->Flush().message().find(cause) != std::string::npos);
    CHECK(enc->RequestKeyFrame().message().find(cause) != std::string::npos);
    CHECK(submit_one(*enc, pixels, 1).message().find(cause) != std::string::npos);

    RuntimeEncoderSettings change;
    change.target_bitrate = 1000000;
    CHECK(enc->Reconfigure(change).message().find(cause) != std::string::npos);

    // Stop is the exception, and deliberately: §8.13 says it is safe from any state, and a
    // caller cleaning up after a failure must not be refused.
    CHECK(enc->Stop().ok());
}

void a_backend_without_rate_control_gets_the_outer_loop()
{
    // §15's loop runs only where the backend has none of its own. Every backend in this
    // build either has one or is guarded away from a target, so this is the only way the
    // loop is exercised at all.
    FakeBackend backend = backend_for_facade();
    backend.native_bitrate = false;
    // H.264, because §50's guard strips a target from an HEVC backend that has no rate
    // control of its own before the loop ever sees it. This is the shape of the real case:
    // vaapi and qsv on H.264 are exactly a backend with a target and no way to honour it.
    backend.serves = Codec::H264;
    backend.reports = Backend::Vaapi;
    // Far above what the target allows, so the loop sees an overshoot on every packet.
    backend.packet_bytes = 400000;
    const BackendRegistry registry = registry_with(backend);
    EncoderConfig cfg = config_for(backend);
    cfg.settings.rate_control.target_bitrate = 200000;

    std::vector<std::pair<LogLevel, std::string>> lines;
    cfg.log = [&lines](LogLevel level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };
    auto enc = CreateVideoEncoderIn(cfg, registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);

    for (int i = 0; i < 200; ++i) {
        if (!submit_one(*enc, pixels, i).ok()) {
            break;
        }
        EncodedPacket packet;
        while (enc->ReceivePacket(&packet).ok()) {
        }
    }

    // §50: the loop running out of authority is a degradation, and one the caller is
    // entitled to hear about -- until this counter existed the only evidence was the file
    // being the wrong size.
    CHECK(enc->GetStatistics().bitrate_target_unreachable);
    CHECK(enc->GetStatistics().fallbacks_applied >= 1);

    int said = 0;
    for (const auto& [level, text] : lines) {
        if (level == LogLevel::Warning
            && text.find("bitrate target unreachable") != std::string::npos) {
            ++said;
        }
    }
    // Once, not once per packet: this runs on every frame, and a forgotten flag would bury
    // the run in its own log.
    CHECK(said == 1);
}

void a_backend_with_its_own_rate_control_is_left_alone()
{
    // The other half of the same rule, and the defect it was written for: two controllers
    // on one stream fight. x265direct went from 79% of target to 99% once the outer loop
    // stopped running on top of its own (docs/STATUS.md, срез Z13).
    FakeBackend backend = backend_for_facade();
    backend.native_bitrate = true;
    backend.serves = Codec::H264;
    backend.reports = Backend::Vaapi;
    backend.packet_bytes = 400000;
    const BackendRegistry registry = registry_with(backend);
    EncoderConfig cfg = config_for(backend);
    cfg.settings.rate_control.target_bitrate = 200000;
    auto enc = CreateVideoEncoderIn(cfg, registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);

    for (int i = 0; i < 200; ++i) {
        if (!submit_one(*enc, pixels, i).ok()) {
            break;
        }
        EncodedPacket packet;
        while (enc->ReceivePacket(&packet).ok()) {
        }
    }
    // Overshooting by the same factor as the case above, and silent: the backend owns the
    // target, so there is nothing here to report or to correct.
    CHECK(!enc->GetStatistics().bitrate_target_unreachable);
}

void a_clamped_quantiser_is_counted_once_and_said_once()
{
    // §50 again, on the counter only a backend can fill: whether a delta added to the
    // frame's own quantiser fell outside the codec's scale is not knowable from here.
    FakeBackend backend = backend_for_facade();
    backend.quantiser_clamped = 7;
    const BackendRegistry registry = registry_with(backend);
    EncoderConfig cfg = config_for(backend);

    std::vector<std::pair<LogLevel, std::string>> lines;
    cfg.log = [&lines](LogLevel level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };
    auto enc = CreateVideoEncoderIn(cfg, registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);

    CHECK(submit_one(*enc, pixels, 0).ok());
    CHECK(submit_one(*enc, pixels, 1).ok());
    CHECK(enc->GetStatistics().frames_quantiser_clamped == 7);
    // Counted as one degradation, not as seven frames and not as two submissions.
    CHECK(enc->GetStatistics().fallbacks_applied == 1);

    int said = 0;
    for (const auto& [level, text] : lines) {
        if (level == LogLevel::Warning && text.find("quantiser") != std::string::npos) {
            ++said;
        }
    }
    CHECK(said == 1);
}

void a_packet_for_a_frame_we_never_held_is_counted_as_an_orphan()
{
    // §19.4: a record has to correspond to a packet. A backend whose packets carry
    // timestamps the submitter never used breaks that, and the gap must be visible rather
    // than leave a sidecar that is merely shorter than the stream.
    FakeBackend backend = backend_for_facade();
    backend.pts_skew = 999999;
    const BackendRegistry registry = registry_with(backend);
    EncoderConfig cfg = config_for(backend);
    std::vector<std::string> records;
    std::vector<std::pair<LogLevel, std::string>> lines;
    cfg.settings.metadata.sidecar = SidecarFormat::NDJSON;
    cfg.settings.metadata.sidecar_sink = [&records](std::string_view line) {
        records.emplace_back(line);
    };
    cfg.log = [&lines](LogLevel level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };
    auto enc = CreateVideoEncoderIn(cfg, registry);
    FakeBuffer pixels(kWidth, kHeight, PixelFormat::YUV420P);

    for (int i = 0; i < 4; ++i) {
        CHECK(submit_one(*enc, pixels, i).ok());
        EncodedPacket packet;
        while (enc->ReceivePacket(&packet).ok()) {
        }
    }
    // Every packet arrived under a timestamp nothing was submitted with, so no record can
    // be written for any of them.
    CHECK(records.empty());
    enc->Stop();

    // Two failures, and the counts must not be added together: four frames went in and
    // four packets came out, so it is four records with no packet and four packets with no
    // record -- not eight of anything. Until 2026-09-04 one number carried both and said
    // eight.
    int said_missing_packet = 0;
    int said_unknown_packet = 0;
    for (const auto& [level, text] : lines) {
        if (level != LogLevel::Warning) {
            continue;
        }
        if (text.find("packet never arrived") != std::string::npos) {
            ++said_missing_packet;
            CHECK(text.find("4 frame(s)") != std::string::npos);
        }
        if (text.find("no frame was submitted with") != std::string::npos) {
            ++said_unknown_packet;
            CHECK(text.find("4 packet(s)") != std::string::npos);
        }
    }
    CHECK(said_missing_packet == 1);
    CHECK(said_unknown_packet == 1);
}

void a_backend_that_cannot_open_refuses_at_create()
{
    FakeBackend backend = backend_for_facade();
    backend.configure_fails = true;
    const BackendRegistry registry = registry_with(backend);

    StatusCode code = StatusCode::Ok;
    try {
        auto enc = CreateVideoEncoderIn(config_for(backend), registry);
        (void)enc;
    } catch (const EncoderError& e) {
        code = e.status().code();
    }
    // The chain came back empty, and that is a refusal at construction rather than an
    // encoder in a state nobody can use.
    CHECK(code != StatusCode::Ok);
}

} // namespace

int main()
{
    a_backend_that_throws_becomes_a_status_not_an_exception();
    every_entry_point_reports_the_failure_that_happened();
    a_backend_without_rate_control_gets_the_outer_loop();
    a_backend_with_its_own_rate_control_is_left_alone();
    a_clamped_quantiser_is_counted_once_and_said_once();
    a_packet_for_a_frame_we_never_held_is_counted_as_an_orphan();
    a_backend_that_cannot_open_refuses_at_create();
    std::printf("encoder_faults_test: ok\n");
    return 0;
}
