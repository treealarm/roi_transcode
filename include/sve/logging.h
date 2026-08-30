#pragma once

// Where the library's diagnostics go -- TZ v3.4 §29.
//
// A library that writes to stderr on its own decides for the application where its logs
// end up, and an application embedded in a service usually has an opinion about that: a
// structured logger, a file, a bus. So the destination is the caller's to name.
//
// It matters more than tidiness. §50 forbids a silent fallback: every degradation this
// library applies -- regions cut to what the hardware takes, a metadata channel the
// encoder cannot carry, a rate-control mode refused -- has to be observable. Observable
// through a pipe the application cannot reach is not much better than not at all.
//
// Leaving the sink unset keeps the old behaviour: one line per event on stderr. That is
// the right default for a command-line tool and for the acceptance scripts, which read
// exactly those lines.

#include <functional>
#include <string_view>

namespace sve {

enum class LogLevel {
    /// Detail useful while chasing something. Off by default in the built-in sink.
    Debug,
    /// Something worth knowing that is not a problem: which backend opened.
    Info,
    /// A degradation was applied. This is the level §50 is about.
    Warning,
    /// The operation failed. The Status returned alongside carries the same message.
    Error,
};

const char* log_level_name(LogLevel level);

/// Called from whichever thread produced the message, and possibly from the encoder's own
/// worker -- an implementation that touches shared state must say so itself. Must not
/// throw; an exception escaping here would unwind through code that has a frame in flight.
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

} // namespace sve
