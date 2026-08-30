#pragma once

// The library's own end of the logging contract -- TZ v3.4 §29.
//
// One place that knows what to do when the application named no sink, so that the answer
// is the same everywhere and no caller writes to stderr directly. Held by value and
// cheap to copy: it is a std::function and a bool.

#include "sve/logging.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace sve {

class Logger {
public:
    Logger() = default;
    explicit Logger(LogSink sink) : m_sink(std::move(sink)) {}

    void write(LogLevel level, std::string_view message) const
    {
        if (m_sink) {
            m_sink(level, message);
            return;
        }
        // No sink: stderr, one line, no level prefix on warnings.
        //
        // The lack of a prefix is not an oversight. These exact lines are what
        // integration/scripts/check.sh reads to prove a guard fired, and they are what a
        // person running the CLI sees. Debug is dropped rather than printed, because the
        // built-in sink has no way to be turned down.
        if (level == LogLevel::Debug) {
            return;
        }
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }

    void info(std::string_view m) const { write(LogLevel::Info, m); }
    void warn(std::string_view m) const { write(LogLevel::Warning, m); }
    void error(std::string_view m) const { write(LogLevel::Error, m); }

private:
    LogSink m_sink;
};

} // namespace sve
