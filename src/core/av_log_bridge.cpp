// Routing FFmpeg's diagnostics into a LogTarget. See ta_roienc/av_log_bridge.h for why this is
// process-wide and why that is the application's decision rather than an encoder's.

#include "ta_roienc/av_log_bridge.h"

#include <cstdarg>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

namespace ta_roienc {

namespace {

/// Deliberately immortal, and never destroyed.
///
/// av_log_set_callback hands FFmpeg a raw function pointer that outlives everything: it is
/// process-wide, and nothing takes it back when this translation unit's statics are
/// destroyed at exit. An ordinary static here would leave `forward` locking a destroyed
/// mutex and copying a destroyed std::function for any av_log that happens during static
/// destruction -- a consumer tearing down a static AVFormatContext, say. Leaked on purpose
/// so that the callback can never outlive what it touches; the leak is one mutex and one
/// std::function, once, for the life of the process.
std::mutex& mutex()
{
    static std::mutex* const m = new std::mutex();
    return *m;
}

LogTarget& sink_slot()
{
    static LogTarget* const s = new LogTarget();
    return *s;
}

/// What av_log_format_line2 is given to write into before it says how much it wanted.
/// Chosen to hold FFmpeg's own longest ordinary lines without a second pass; anything
/// longer takes the resize below, so the number is a starting point rather than a limit.
constexpr int kInitialLineBytes = 1024;

/// The severity, with FFmpeg's colour tint removed.
///
/// av_log's level argument carries a tint in its high bits -- AV_LOG_C(x) is (x << 8) --
/// and every consumer inside FFmpeg masks it off before doing anything (av_log_format_line2
/// among them). Comparing or mapping the raw value instead means a tinted AV_LOG_ERROR
/// arrives as 50192: it fails the level test and vanishes from the sink and from stderr
/// both, and were it to pass, it would be classified as Trace. Masked once, here, so that
/// neither the filter below nor the mapping can see the tint.
int severity_of(int av_level) { return av_level & 0xff; }

Verbosity level_from_av(int av_level)
{
    // libavutil/log.h: PANIC 0, FATAL 8, ERROR 16, WARNING 24, INFO 32, VERBOSE 40,
    // DEBUG 48, TRACE 56. Compared rather than switched, because FFmpeg's own macros put
    // levels between those values and a switch would drop them on the floor.
    if (av_level <= AV_LOG_ERROR) {
        return Verbosity::Error;
    }
    if (av_level <= AV_LOG_WARNING) {
        return Verbosity::Warning;
    }
    if (av_level <= AV_LOG_INFO) {
        return Verbosity::Info;
    }
    if (av_level <= AV_LOG_DEBUG) {
        return Verbosity::Debug;
    }
    return Verbosity::Trace;
}

void forward(void* avcl, int level, const char* fmt, va_list args)
{
    // Asked before anything is formatted: FFmpeg calls this for every message it considers,
    // including the ones the current av_log level discards, and formatting one only to drop
    // it costs the encode for nothing. The comparison is on the masked severity, as
    // av_log_default_callback's own is.
    const int severity = severity_of(level);
    if (severity > av_log_get_level()) {
        return;
    }

    LogTarget sink;
    {
        const std::lock_guard<std::mutex> lock(mutex());
        if (!sink_slot()) {
            return;
        }
        sink = sink_slot();
    }

    // FFmpeg's own formatter, so that the "[libx264 @ 0x...]" prefix and the line-continuation
    // handling are the ones a reader already recognises. `print_prefix` is per-call state
    // that FFmpeg carries between fragments of one line; a fresh one each time means every
    // message arrives with its prefix, which is what a structured sink wants -- it has no
    // way to reassemble fragments it never asked for.
    std::vector<char> line(kInitialLineBytes);
    int print_prefix = 1;
    // The formatter consumes the argument list, and there is a second call below, so it is
    // copied rather than passed twice -- reusing a va_list after it has been walked is
    // undefined and happens to work until it does not.
    va_list retry;
    va_copy(retry, args);
    const int wanted = av_log_format_line2(
        avcl, level, fmt, args, line.data(), static_cast<int>(line.size()), &print_prefix);
    if (wanted >= static_cast<int>(line.size())) {
        line.resize(static_cast<std::size_t>(wanted) + 1);
        print_prefix = 1;
        av_log_format_line2(avcl, level, fmt, retry, line.data(),
            static_cast<int>(line.size()), &print_prefix);
    }
    va_end(retry);
    if (wanted <= 0) {
        return;
    }

    std::string text(line.data());
    // FFmpeg ends its lines; a sink frames its own. Trailing rather than all, so a message
    // that is genuinely several lines stays several lines.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    sink(level_from_av(severity), text);
}

} // namespace

void SetFFmpegLogSink(LogTarget sink)
{
    const bool wanted = static_cast<bool>(sink);
    {
        const std::lock_guard<std::mutex> lock(mutex());
        sink_slot() = std::move(sink);
    }
    // The sink is swapped under the lock first, then the callback. A message arriving from
    // another thread mid-swap therefore finds a consistent sink or none -- never a dangling
    // one. Clearing can lose a message in that window, which is the right trade: the
    // alternative is holding the lock across a call into application code.
    av_log_set_callback(wanted ? &forward : &av_log_default_callback);
}

} // namespace ta_roienc
