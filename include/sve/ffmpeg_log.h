#pragma once

// FFmpeg's own diagnostics, routed into a sink the application named.
//
// FFmpeg writes to stderr by default, from inside libavcodec and from inside the codec
// libraries below it -- the "[libx264 @ 0x...]" lines. An application that gave this library
// a LogSink still saw those go somewhere it does not control, which is precisely the thing
// sve/logging.h exists to prevent.
//
// **It is a process-wide setting, and that is why it is here rather than on the encoder.**
// av_log_set_callback installs one callback for the whole process, while a LogSink belongs
// to one encoder, and this library supports many encoders at once on many threads (see
// sve/encoder.h). There is no honest mapping from the one to the many: FFmpeg logs from its
// own worker threads, so not even a thread-local "current encoder" would catch them.
//
// So the process-global thing gets a process-global owner: the application, which is the
// only party that knows there is one process. Calling this is opt-in and it affects
// everything in the process that uses FFmpeg, including code that has nothing to do with
// this library. That is stated plainly rather than hidden behind an encoder option that
// would look narrower than it is.
//
// Linking: the implementation lives in the FFmpeg interop component, so a build without
// SVE_BUILD_FFMPEG_INTEROP has this declaration and no definition. The header itself names
// no FFmpeg type and needs no FFmpeg include path -- sve/frame.h stays the only public
// header that mentions one.

#include "sve/logging.h"

namespace sve {

/// Sends FFmpeg's diagnostics to `sink` for the whole process.
///
/// Levels are mapped onto this library's own: AV_LOG_PANIC through AV_LOG_ERROR become
/// Error, AV_LOG_WARNING becomes Warning, AV_LOG_INFO becomes Info, AV_LOG_VERBOSE and
/// AV_LOG_DEBUG become Debug, and AV_LOG_TRACE becomes Trace. Messages arrive one line at
/// a time with the trailing newline removed, so a sink that adds its own framing does not
/// have to strip anything.
///
/// **The last two of those mappings need FFmpeg's own level raised first.** Installing a
/// sink does not change what FFmpeg is willing to say: it discards anything above
/// av_log_get_level(), which defaults to AV_LOG_INFO, so Debug and Trace never arrive
/// unless the application calls av_log_set_level(AV_LOG_DEBUG) or higher itself. That is
/// left to the application on purpose -- the level is process-wide exactly as this callback
/// is, and a library that quietly raised it would be turning on a firehose in somebody
/// else's process. It is documented here because a mapping the caller cannot reach reads
/// as a defect otherwise.
///
/// FFmpeg calls its callback from any thread, including ones this library never created,
/// so `sink` must tolerate that; the sink is held behind a lock here, but what the sink
/// itself touches is the sink's own business.
///
/// An empty `sink` restores FFmpeg's default, which is its own line on stderr.
void SetFFmpegLogSink(LogSink sink);

} // namespace sve
