// The events §29 requires, and the sink they are required to reach.
//
// §29 lists five levels and seven mandatory events. Until 2026-09-02 the list lived only in
// the specification: three of the events were emitted, one was emitted only as a Status the
// log never saw, and two did not exist. Nothing anywhere held the library to the list, so
// nothing noticed.
//
// This file is that hold. Each test names the event from §29 it covers, so that an event
// removed from the code shows up here as a failure rather than as a line nobody missed.
//
// It also covers the FFmpeg routing of sve/ffmpeg_log.h, which is the other half of §29:
// diagnostics that go to stderr behind the application's back are not observable through
// the sink it named, and a log with a hole in it is what §50 is arguing against.

#include "encoder_runtime_warnings.h"
#include "sve/encoder.h"
#include "sve/ffmpeg_log.h"
#include "sve/logging.h"
#include "sve/raw_frame.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

using namespace sve;

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
struct Captured {
    std::vector<std::pair<LogLevel, std::string>> lines;

    LogSink sink()
    {
        return [this](LogLevel level, std::string_view message) {
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

    bool said_at(LogLevel want, std::string_view needle) const
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
            std::fprintf(stderr, "    [%s] %s\n", log_level_name(level), text.c_str());
        }
    }
};

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 96;

EncoderConfig config_with(LogSink sink)
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = Backend::X264Direct;
    cfg.settings.codec = Codec::H264;
    cfg.settings.crf = 32;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = std::move(sink);
    return cfg;
}

// --- §29's five levels ------------------------------------------------------

/// The list is normative and it is five long. Trace was missing until 2026-09-02.
void every_level_the_spec_names_exists_and_has_a_name()
{
    check(std::string(log_level_name(LogLevel::Trace)) == "trace", "TRACE has a name");
    check(std::string(log_level_name(LogLevel::Debug)) == "debug", "DEBUG has a name");
    check(std::string(log_level_name(LogLevel::Info)) == "info", "INFO has a name");
    check(std::string(log_level_name(LogLevel::Warning)) == "warning", "WARN has a name");
    check(std::string(log_level_name(LogLevel::Error)) == "error", "ERROR has a name");
}

// --- §29's mandatory events -------------------------------------------------

/// "backend selected" and "capability report", both at open.
///
/// The capability report is the one worth explaining: every fallback below is decided
/// against these numbers, so a log that recorded the fallbacks and not the capabilities
/// they came from could not be read back to a cause.
void opening_reports_the_backend_and_what_it_can_do()
{
    Captured log;
    const std::unique_ptr<SmartVideoEncoder> enc = CreateVideoEncoder(config_with(log.sink()));
    check(enc != nullptr, "the encoder opened");
    if (!log.said_at(LogLevel::Info, "backend: x264direct")) {
        log.dump("open");
    }
    check(log.said_at(LogLevel::Info, "backend: x264direct"), "§29 backend selected");
    check(log.said_at(LogLevel::Debug, "capabilities:"), "§29 capability report");
    check(log.said("qp_map="), "the capability report carries the fields, not just a header");
}

/// "encoder reset/reconfigure" -- both halves, and reconfigure names what moved.
void reconfigure_and_stop_are_announced()
{
    Captured log;
    const std::unique_ptr<SmartVideoEncoder> enc = CreateVideoEncoder(config_with(log.sink()));

    RuntimeEncoderSettings change;
    change.quality = Quality{};
    check(enc->Reconfigure(change).ok(), "the encoder took the change");
    check(log.said_at(LogLevel::Info, "reconfigured: quality"), "§29 reconfigure names it");
    check(!log.said("reconfigured: frame_rate"),
        "and names only what moved -- frame rate was not set");

    check(enc->Stop().ok(), "the encoder stopped");
    check(log.said_at(LogLevel::Info, "encoder stopped"), "§29 encoder reset");
    check(log.said("frames in"), "the reset carries the counts a reader wants");
}

/// "fallback" -- a degradation applied under Permissive, announced once.
void a_fallback_is_announced_and_only_once()
{
    Captured log;
    const Logger logger(log.sink());
    EncoderCapabilities caps;
    caps.sei = false;
    bool warned = false;

    check(guard_sei_unsupported(true, caps, FallbackPolicy::Permissive, logger, warned).ok(),
        "Permissive applies the degradation rather than refusing");
    check(log.said_at(LogLevel::Warning, "bitstream SEI"), "§29 fallback");
    const std::size_t after_first = log.lines.size();

    check(guard_sei_unsupported(true, caps, FallbackPolicy::Permissive, logger, warned).ok(),
        "still fine the second time");
    check(log.lines.size() == after_first,
        "announced once: a warning repeated per frame is a warning nobody reads");
}

/// "unsupported feature" -- refused under Strict, and the refusal reaches the log.
///
/// The regression this pins: until 2026-09-02 a Strict refusal returned a Status and said
/// nothing. The caller knew; anyone reading the log to find out why an encode stopped did
/// not, which is exactly the observability §50 asks for and does not get from a return value.
void a_strict_refusal_reaches_the_log_and_not_only_the_caller()
{
    Captured log;
    const Logger logger(log.sink());
    EncoderCapabilities caps;
    caps.sei = false;
    bool warned = false;

    const Status s = guard_sei_unsupported(true, caps, FallbackPolicy::Strict, logger, warned);
    check(s.code() == StatusCode::Unsupported, "Strict refuses");
    check(log.said_at(LogLevel::Error, "unsupported:"), "§29 unsupported feature");
    // One text for both, so the two can never describe different refusals.
    check(log.said(s.message()), "the log carries the same words as the Status");
}

/// "metadata failure" -- the metadata channel could not carry what was asked for.
void a_metadata_channel_that_cannot_carry_says_so()
{
    Captured log;
    const Logger logger(log.sink());
    EncoderCapabilities caps;
    caps.sei = false;
    bool warned = false;
    (void)guard_sei_unsupported(true, caps, FallbackPolicy::Permissive, logger, warned);
    check(log.said("SEI requested but not supported"), "§29 metadata failure");
}

// --- the other half of §29: FFmpeg's own diagnostics ------------------------

/// FFmpeg logs into the sink, at the mapped level, with its newline removed.
void ffmpeg_diagnostics_arrive_through_the_sink()
{
    Captured log;
    const int previous = av_log_get_level();
    av_log_set_level(AV_LOG_DEBUG);
    SetFFmpegLogSink(log.sink());

    av_log(nullptr, AV_LOG_WARNING, "sve logging test: %d\n", 42);
    av_log(nullptr, AV_LOG_ERROR, "sve logging test error\n");

    check(log.said_at(LogLevel::Warning, "sve logging test: 42"),
        "an FFmpeg warning arrives, formatted, at Warning");
    check(log.said_at(LogLevel::Error, "sve logging test error"),
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

    if (failures > 0) {
        std::fprintf(stderr, "logging_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("logging_test ok");
    return 0;
}
