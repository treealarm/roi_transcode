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
    /// Per-frame detail, and the level §29 names below Debug. Off by default in the
    /// built-in sink: at a frame's granularity this is a firehose, and the only honest
    /// destination for it is a sink the application chose on purpose.
    Trace,
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

/// The events §29 requires a library to emit, so that the list is in the code rather than
/// only in the specification. Every one of them is emitted through a Logger. How far each
/// is held by a test differs, and is said here rather than left to be assumed:
///
///   * backend selected          -- which one opened and how many declined first.
///                                  Covered through a real encoder.
///   * capability report         -- what the one that opened says it can do.
///                                  Covered through a real encoder.
///   * encoder reset/reconfigure -- Reconfigure and Stop.
///                                  Covered through a real encoder.
///   * fallback                  -- a degradation was applied (§50).
///                                  Covered at the guard that emits it, not through an
///                                  encoder: provoking one that way needs a backend
///                                  lacking the capability, and every backend in this
///                                  build has it.
///   * unsupported feature       -- something was refused rather than worked around.
///                                  Covered at the guard, for the same reason.
///   * metadata failure          -- a metadata channel could not carry what was asked.
///                                  Covered at the guard, for the same reason.
///   * frame drop                -- see below. Emitted, and not forced by any test.
///
/// On frame drop: this library never drops a submitted frame. Back pressure is `Again`
/// from SubmitFrame, which is the caller's frame still in the caller's hands, not a loss.
/// What can happen instead is a frame whose packet never arrives to claim it, and that is
/// the event reported under this name. Nothing forces it: it needs an encoder that accepts
/// a frame and then never emits a packet carrying its timestamp, which no backend here
/// does. The acceptance step for §19.3 checks the condition it reports on -- that records
/// and packets correspond -- rather than the warning that fires when they do not.

/// Called from whichever thread produced the message, and possibly from the encoder's own
/// worker -- an implementation that touches shared state must say so itself. Must not
/// throw; an exception escaping here would unwind through code that has a frame in flight.
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

} // namespace sve
