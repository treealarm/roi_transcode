// The log events the library emits, and the sink they are required to reach.
//
// Each test names the event it covers, so that an event removed from the code shows up here
// as a failure rather than as a line nobody missed.
//
// It also covers the FFmpeg routing of ta_roienc/av_log_bridge.h: diagnostics that go to stderr
// behind the application's back are not observable through the sink it named, and a log
// with a hole in it is what this whole contract is arguing against.

#include "live_tuning_warnings.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/av_log_bridge.h"
#include "ta_roienc/log.h"
#include "ta_roienc/picture_view.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

using namespace ta_roienc;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

/// Everything the library said, with the level it said it at.
struct Caught {
    std::vector<std::pair<Verbosity, std::string>> lines;

    LogTarget sink()
    {
        return [this](Verbosity level, std::string_view message) {
            lines.emplace_back(level, std::string(message));
        };
    }

    /// Whether any line at all contains `needle`. Substring rather than equality on
    /// purpose: these messages carry counts and names that change, and a test that pinned
    /// the whole sentence would fail on every rewording while proving nothing more.
    bool said(std::string_view needle) const
    {
        for (const auto& [level, text] : lines) {
            if (text.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool said_at(Verbosity want, std::string_view needle) const
    {
        for (const auto& [level, text] : lines) {
            if (level == want && text.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void dump(const char* label) const
    {
        std::fprintf(stderr, "  %s captured %zu line(s):\n", label, lines.size());
        for (const auto& [level, text] : lines) {
            std::fprintf(stderr, "    [%s] %s\n", verbosity_name(level), text.c_str());
        }
    }
};

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 96;

EncodeOptions config_with(LogTarget sink)
{
    EncodeOptions cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Ratio{25, 1};
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.encoder = Engine::X264Direct;
    cfg.settings.codec = Coding::H264;
    cfg.settings.crf = 32;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = std::move(sink);
    return cfg;
}

// --- the five levels ---------------------------------------------------------

/// The list is five long.
void every_level_the_spec_names_exists_and_has_a_name()
{
    check(std::string(verbosity_name(Verbosity::Trace)) == "trace", "TRACE has a name");
    check(std::string(verbosity_name(Verbosity::Debug)) == "debug", "DEBUG has a name");
    check(std::string(verbosity_name(Verbosity::Info)) == "info", "INFO has a name");
    check(std::string(verbosity_name(Verbosity::Warning)) == "warning", "WARN has a name");
    check(std::string(verbosity_name(Verbosity::Error)) == "error", "ERROR has a name");
}

// --- the events ------------------------------------------------------------

/// "backend selected" and "capability report", both at open.
///
/// The capability report is the one worth explaining: every fallback below is decided
/// against these numbers, so a log that recorded the fallbacks and not the capabilities
/// they came from could not be read back to a cause.
void opening_reports_the_backend_and_what_it_can_do()
{
    Caught log;
    const std::unique_ptr<VideoEncoder> enc = CreateVideoEncoder(config_with(log.sink()));
    check(enc != nullptr, "the encoder opened");
    if (!log.said_at(Verbosity::Info, "backend: x264direct")) {
        log.dump("open");
    }
    check(log.said_at(Verbosity::Info, "backend: x264direct"), "backend selected");
    check(log.said_at(Verbosity::Debug, "capabilities:"), "capability report");
    check(log.said("qp_map="), "the capability report carries the fields, not just a header");
}

/// "encoder reset/reconfigure" -- both halves, and reconfigure names what moved.
void reconfigure_and_stop_are_announced()
{
    Caught log;
    const std::unique_ptr<VideoEncoder> enc = CreateVideoEncoder(config_with(log.sink()));

    LiveTuning change;
    change.quality = QualityPlan{};
    check(enc->Reconfigure(change).ok(), "the encoder took the change");
    check(log.said_at(Verbosity::Info, "reconfigured: quality"), "reconfigure names it");
    check(!log.said("reconfigured: frame_rate"),
        "and names only what moved -- frame rate was not set");

    check(enc->Stop().ok(), "the encoder stopped");
    check(log.said_at(Verbosity::Info, "encoder stopped"), "encoder reset");
    check(log.said("frames in"), "the reset carries the counts a reader wants");
}

/// "fallback" -- a degradation applied under Permissive, announced once.
void a_fallback_is_announced_and_only_once()
{
    Caught log;
    const Journal logger(log.sink());
    EngineAbilities caps;
    caps.sei = false;
    bool warned = false;

    check(guard_sei_unsupported(true, caps, EngineFallback::Permissive, logger, warned).ok(),
        "Permissive applies the degradation rather than refusing");
    check(log.said_at(Verbosity::Warning, "bitstream SEI"), "fallback");
    const std::size_t after_first = log.lines.size();

    check(guard_sei_unsupported(true, caps, EngineFallback::Permissive, logger, warned).ok(),
        "still fine the second time");
    check(log.lines.size() == after_first,
        "announced once: a warning repeated per frame is a warning nobody reads");
}

/// "unsupported feature" -- refused under Strict, and the refusal reaches the log.
///
/// A Verdict alone tells the caller; anyone reading the log to find out why an encode
/// stopped needs the log line too.
void a_strict_refusal_reaches_the_log_and_not_only_the_caller()
{
    Caught log;
    const Journal logger(log.sink());
    EngineAbilities caps;
    caps.sei = false;
    bool warned = false;

    const Verdict s = guard_sei_unsupported(true, caps, EngineFallback::Strict, logger, warned);
    check(s.code() == VerdictCode::Unsupported, "Strict refuses");
    check(log.said_at(Verbosity::Error, "unsupported:"), "unsupported feature");
    // One text for both, so the two can never describe different refusals.
    check(log.said(s.message()), "the log carries the same words as the Verdict");
}

/// "metadata failure" -- the metadata channel could not carry what was asked for.
void a_metadata_channel_that_cannot_carry_says_so()
{
    Caught log;
    const Journal logger(log.sink());
    EngineAbilities caps;
    caps.sei = false;
    bool warned = false;
    (void)guard_sei_unsupported(true, caps, EngineFallback::Permissive, logger, warned);
    check(log.said("SEI requested but not supported"), "metadata failure");
}

// --- FFmpeg's own diagnostics ----------------------------------------------

/// FFmpeg logs into the sink, at the mapped level, with its newline removed.
void ffmpeg_diagnostics_arrive_through_the_sink()
{
    Caught log;
    const int previous = av_log_get_level();
    av_log_set_level(AV_LOG_DEBUG);
    SetFFmpegLogSink(log.sink());

    av_log(nullptr, AV_LOG_WARNING, "ta_roienc logging test: %d\n", 42);
    av_log(nullptr, AV_LOG_ERROR, "ta_roienc logging test error\n");

    check(log.said_at(Verbosity::Warning, "ta_roienc logging test: 42"),
        "an FFmpeg warning arrives, formatted, at Warning");
    check(log.said_at(Verbosity::Error, "ta_roienc logging test error"),
        "and an FFmpeg error arrives at Error");
    for (const auto& [level, text] : log.lines) {
        (void)level;
        check(text.empty() || text.back() != '\n',
            "the trailing newline is stripped: a sink frames its own lines");
    }

    // Restored, because this is process-wide and every test after it shares the process.
    // The sink captured above is about to go out of scope, so leaving it installed would
    // hand FFmpeg a dangling one.
    SetFFmpegLogSink(nullptr);
    av_log_set_level(previous);

    const std::size_t settled = log.lines.size();
    av_log(nullptr, AV_LOG_ERROR, "this one must not be captured\n");
    check(log.lines.size() == settled, "clearing the sink restores FFmpeg's own default");
}

/// A tinted level is still the level it is.
///
/// av_log carries a colour tint in the high bits of its level -- AV_LOG_C(x) is (x << 8) --
/// and every consumer inside FFmpeg masks it off before doing anything. Compared and mapped
/// raw, a tinted AV_LOG_ERROR arrives as 50192: it fails the level test and disappears from
/// the sink and from stderr both, and had it passed, it would be classified as Trace.
void a_tinted_level_is_still_the_level_it_is()
{
    Caught log;
    SetFFmpegLogSink(log.sink());

    av_log(nullptr, AV_LOG_ERROR | AV_LOG_C(196), "ta_roienc logging test tinted\n");

    if (!log.said_at(Verbosity::Error, "ta_roienc logging test tinted")) {
        log.dump("tinted");
    }
    check(log.said_at(Verbosity::Error, "ta_roienc logging test tinted"),
        "a tinted error is delivered, and delivered as an error");

    SetFFmpegLogSink(nullptr);
}

} // namespace

int main()
{
    every_level_the_spec_names_exists_and_has_a_name();
    opening_reports_the_backend_and_what_it_can_do();
    reconfigure_and_stop_are_announced();
    a_fallback_is_announced_and_only_once();
    a_strict_refusal_reaches_the_log_and_not_only_the_caller();
    a_metadata_channel_that_cannot_carry_says_so();
    ffmpeg_diagnostics_arrive_through_the_sink();
    a_tinted_level_is_still_the_level_it_is();

    if (failures > 0) {
        std::fprintf(stderr, "logging_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("logging_test ok");
    return 0;
}
