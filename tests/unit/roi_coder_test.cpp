// What the pipeline does when a backend fails.
//
// Everything else in tests/unit drives the pipeline through a backend that works, because
// the backends here do work: libx264 opens on any machine. That leaves the other half of
// roi_coder.cpp -- the branch taken when Configure declines, when a probe opens and
// encodes nothing, when GetCodingParams has no answer, when SubmitPicture or Flush
// refuses. Those branches are the ones that run where the hardware is missing or busy.
//
// A fake backend rather than a real one under induced load: an interface's failure modes
// are the return values it declares. Making libx264 fail on demand would mean a switch
// inside shipping code, which is the thing this file exists to avoid -- RoiCoder takes
// the registry it opens from, so the fake arrives the same way any component does. The
// fake itself is in tests/unit/stub_engine.h, shared with encode_faults_test.

#include "roi_coder.h"

#include "stub_engine.h"
#include "access_unit_outlet.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
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

using testing::StubEngine;
using testing::StubStore;
using testing::registry_with;

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 48;
constexpr int64_t kTicksPerFrame = StubEngine::kTicksPerFrame;

EngineSetup config_for(const StubEngine& backend)
{
    EngineSetup config;
    config.input.width = backend.width;
    config.input.height = backend.height;
    config.input.frame_rate = Ratio{StubEngine::kFrameRate, 1};
    config.input.pixel_layout = PixelLayout::NV12;
    config.codec = backend.serves;
    config.kind = backend.kind();
    return config;
}

class TallySink : public AccessUnitOutlet {
public:
    void begin(const CodingParams& params) override
    {
        ++begins;
        width = params.width;
    }
    void on_access_unit(const AccessUnit& packet) override
    {
        ++packets;
        last_duration = packet.duration;
        last_time_base = packet.time_base;
    }
    void end() override { ++ends; }

    int begins = 0;
    int packets = 0;
    int ends = 0;
    int32_t width = 0;
    int64_t last_duration = -1;
    Ratio last_time_base{0, 0};
};

/// What open_deferred did, or the message it threw. Every refusal below is an exception by
/// design -- open() is the one call in this class that cannot return a status, because the
/// object has no valid state to be left in.
std::string refusal_of(RoiCoder& encoder, const EngineSetup& config)
{
    try {
        encoder.open_deferred(config);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

void a_working_backend_opens_and_delivers()
{
    StubEngine backend;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;

    encoder.open(config_for(backend), sink);
    CHECK(encoder.kind() == Engine::X265Direct);
    CHECK(sink.begins == 1);
    CHECK(sink.width == kWidth);
    CHECK(std::string(encoder.label()) == "libx265 (CPU, direct API, per-block quantOffsets)");

    const auto made = encoder.create_frame(MemoryKind::CPU);
    CHECK(made);
    CHECK(made.value()->width() == kWidth);

    StubStore frame(kWidth, kHeight, PixelLayout::NV12);
    encoder.encode_buffer(frame, {}, 0);
    encoder.flush();
    CHECK(sink.packets == 1);
    CHECK(encoder.backends_declined() == 0);
}

void a_backend_that_cannot_serve_the_codec_leaves_its_reason()
{
    StubEngine backend;
    backend.serves = Coding::H265;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    EngineSetup config = config_for(backend);
    config.codec = Coding::H264;

    const std::string why = refusal_of(encoder, config);
    // Skipped is not the same as tried and declined, and the count must not blur them -- no
    // device was touched here.
    CHECK(encoder.backends_declined() == 0);
    CHECK(encoder.backends_skipped().size() == 1);
    CHECK(encoder.backends_skipped()[0].find("H.264") != std::string::npos);
    CHECK(why.find("nothing that can encode") != std::string::npos);
}

void a_backend_that_declines_is_counted_and_quoted()
{
    StubEngine backend;
    backend.configure_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    const std::string why = refusal_of(encoder, config_for(backend));
    CHECK(encoder.backends_declined() == 1);
    CHECK(why.find("HEVC") != std::string::npos);
    CHECK(why.find("told to decline") != std::string::npos);
}

void a_probe_that_encodes_nothing_declines_the_backend()
{
    StubEngine backend;
    backend.needs_probe = true;
    backend.probe_encodes_nothing = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    const std::string why = refusal_of(encoder, config_for(backend));
    CHECK(encoder.backends_declined() == 1);
    CHECK(why.find("opened but encodes nothing") != std::string::npos);
    CHECK(backend.submits == 1);
}

void a_probe_that_succeeds_opens_a_second_untouched_session()
{
    StubEngine backend;
    backend.needs_probe = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    CHECK(refusal_of(encoder, config_for(backend)).empty());
    // Twice: the probe consumed the first session, so the one that is kept is fresh.
    CHECK(backend.configures == 2);
    CHECK(encoder.backends_declined() == 0);
}

void a_backend_whose_probe_frame_never_arrives_declines()
{
    StubEngine backend;
    backend.needs_probe = true;
    backend.create_frame_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    CHECK(refusal_of(encoder, config_for(backend)).find("encodes nothing")
        != std::string::npos);
    CHECK(backend.submits == 0);
}

void a_probe_the_backend_will_not_take_a_frame_for_declines()
{
    StubEngine backend;
    backend.needs_probe = true;
    backend.submit_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    CHECK(refusal_of(encoder, config_for(backend)).find("encodes nothing")
        != std::string::npos);
    CHECK(backend.submits == 1);
}

void a_probe_whose_flush_fails_declines()
{
    // The probe submits one frame and gets Again, which is normal -- an encoder with a
    // lookahead has nothing to give back yet. Flush is what makes it answer, so a flush
    // that fails leaves the question unanswered, and an unanswered probe is a decline.
    StubEngine backend;
    backend.needs_probe = true;
    backend.probe_encodes_nothing = true;
    backend.flush_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    CHECK(refusal_of(encoder, config_for(backend)).find("encodes nothing")
        != std::string::npos);
    CHECK(encoder.backends_declined() == 1);
}

void a_reopen_that_fails_after_a_good_probe_is_not_counted_twice()
{
    StubEngine backend;
    backend.needs_probe = true;
    backend.reopen_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    const std::string why = refusal_of(encoder, config_for(backend));
    CHECK(why.find("told to decline") != std::string::npos);
    // The backend was tried once and declined once: the probe succeeded, so the second
    // Configure failing is the same attempt ending, not a new one.
    CHECK(encoder.backends_declined() == 0);
    CHECK(backend.configures == 2);
}

void a_backend_that_cannot_describe_itself_is_refused()
{
    StubEngine backend;
    backend.no_codec_parameters = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    CHECK(refusal_of(encoder, config_for(backend)).find("cannot describe itself")
        != std::string::npos);
}

void a_bare_packet_gets_the_duration_the_frame_rate_implies()
{
    StubEngine backend;
    backend.packets_are_bare = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;
    encoder.open(config_for(backend), sink);

    StubStore frame(kWidth, kHeight, PixelLayout::NV12);
    encoder.encode_buffer(frame, {}, 0);
    CHECK(sink.packets == 1);
    // 90 kHz over 30 fps. Without this the mp4 muxer drops the last sample, which is the
    // defect the fallback was written for.
    CHECK(sink.last_duration == kTicksPerFrame);
    CHECK(sink.last_time_base.den == StubEngine::kTimeBaseDen);
}

void a_frame_rate_too_fast_for_the_time_base_still_gets_a_duration()
{
    StubEngine backend;
    backend.packets_are_bare = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;
    EngineSetup config = config_for(backend);
    // A frame shorter than half a tick rounds to zero, and a zero duration is the very
    // thing the fallback exists to prevent -- so it floors at one tick instead.
    config.input.frame_rate = Ratio{StubEngine::kTimeBaseDen * 5, 1};
    encoder.open(config, sink);

    StubStore frame(kWidth, kHeight, PixelLayout::NV12);
    encoder.encode_buffer(frame, {}, 0);
    CHECK(sink.last_duration == 1);
}

void a_submit_that_fails_reaches_the_caller()
{
    StubEngine backend;
    backend.submit_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;
    encoder.open(config_for(backend), sink);

    StubStore frame(kWidth, kHeight, PixelLayout::NV12);
    bool threw = false;
    try {
        encoder.encode_buffer(frame, {}, 0);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("refused the frame") != std::string::npos;
    }
    CHECK(threw);
}

void a_flush_that_fails_reaches_the_caller()
{
    StubEngine backend;
    backend.flush_fails = true;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;
    encoder.open(config_for(backend), sink);

    bool threw = false;
    try {
        encoder.flush();
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("refused to flush") != std::string::npos;
    }
    CHECK(threw);
}

void a_receive_error_is_not_mistaken_for_end_of_stream()
{
    StubEngine backend;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);
    TallySink sink;
    encoder.open(config_for(backend), sink);

    // The drain loop ends quietly on Again and EndOfStream and must not do so on anything
    // else: a lost bitstream that reads as a finished one is a truncated file nobody is
    // told about.
    const_cast<StubEngine&>(backend).receive_fails = true;
    StubStore frame(kWidth, kHeight, PixelLayout::NV12);
    bool threw = false;
    try {
        encoder.encode_buffer(frame, {}, 0);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("lost the bitstream") != std::string::npos;
    }
    CHECK(threw);
}

void an_encoder_that_never_opened_hands_out_no_frames()
{
    StubEngine backend;
    const EngineCatalog registry = registry_with(backend);
    RoiCoder encoder(registry);

    // Before open there is no session to allocate from, and answering with a null buffer
    // would move the crash into the caller.
    const auto made = encoder.create_frame(MemoryKind::CPU);
    CHECK(!made);
    CHECK(made.status().message().find("no encoder is open") != std::string::npos);
    CHECK(encoder.capabilities().h264 == false);
    CHECK(!encoder.effective_settings().b_frames.has_value());
}

void an_empty_registry_says_so_rather_than_naming_a_backend()
{
    const EngineCatalog empty;
    RoiCoder encoder(empty);
    StubEngine backend;
    EngineSetup config = config_for(backend);
    config.kind = Engine::Auto;
    config.engine_order.allow_unlisted = true;

    const std::string why = refusal_of(encoder, config);
    CHECK(why.find("excludes every backend this build has") != std::string::npos);
}

void each_backend_identifier_gets_its_own_label()
{
    // label() is what the log and the capability report print, and every entry in it is a
    // string a reader is expected to recognise. A missing case falls through to "libx264
    // (CPU)", which is not wrong-looking enough to be noticed.
    const struct {
        Engine kind;
        Coding codec;
        const char* expected;
    } cases[] = {
        {Engine::IntelOneVpl, Coding::H264, "h264_qsv (Intel VPL mfxExtEncoderROI)"},
        {Engine::IntelOneVpl, Coding::H265, "hevc_qsv (Intel VPL mfxExtEncoderROI)"},
        {Engine::Vaapi, Coding::H264, "h264_vaapi (Intel iHD VAEncROI)"},
        {Engine::Vaapi, Coding::H265, "hevc_vaapi (Intel iHD VAEncROI)"},
        {Engine::X264, Coding::H264, "libx264 (CPU, via libavcodec)"},
        {Engine::X264Direct, Coding::H264,
            "libx264 (CPU, direct API, per-macroblock quant_offsets)"},
        {Engine::X265, Coding::H265, "libx265 (CPU, per-block quantOffsets)"},
        {Engine::X265Direct, Coding::H265,
            "libx265 (CPU, direct API, per-block quantOffsets)"},
        {Engine::IntelOneVplDirect, Coding::H264, "h264 (Intel oneVPL, direct API, mfxExtMBQP)"},
        {Engine::IntelOneVplDirect, Coding::H265, "hevc (Intel oneVPL, direct API, mfxExtMBQP)"},
    };
    for (const auto& one : cases) {
        StubEngine backend;
        backend.reports = one.kind;
        backend.serves = one.codec;
        const EngineCatalog registry = registry_with(backend);
        RoiCoder encoder(registry);
        CHECK(refusal_of(encoder, config_for(backend)).empty());
        CHECK(std::string(encoder.label()) == one.expected);
    }
}

} // namespace

int main()
{
    a_working_backend_opens_and_delivers();
    a_backend_that_cannot_serve_the_codec_leaves_its_reason();
    a_backend_that_declines_is_counted_and_quoted();
    a_probe_that_encodes_nothing_declines_the_backend();
    a_probe_that_succeeds_opens_a_second_untouched_session();
    a_backend_whose_probe_frame_never_arrives_declines();
    a_probe_the_backend_will_not_take_a_frame_for_declines();
    a_probe_whose_flush_fails_declines();
    a_reopen_that_fails_after_a_good_probe_is_not_counted_twice();
    a_backend_that_cannot_describe_itself_is_refused();
    a_bare_packet_gets_the_duration_the_frame_rate_implies();
    a_frame_rate_too_fast_for_the_time_base_still_gets_a_duration();
    a_submit_that_fails_reaches_the_caller();
    a_flush_that_fails_reaches_the_caller();
    a_receive_error_is_not_mistaken_for_end_of_stream();
    an_encoder_that_never_opened_hands_out_no_frames();
    an_empty_registry_says_so_rather_than_naming_a_backend();
    each_backend_identifier_gets_its_own_label();
    std::printf("roi_encoder_test: ok\n");
    return 0;
}
