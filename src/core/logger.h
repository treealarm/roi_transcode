#pragma once

// The library's own end of the logging contract.
//
// One place that knows what to do when the application named no sink, so that the answer
// is the same everywhere and no caller writes to stderr directly. Held by value and
// cheap to copy: it is a std::function and a bool.

#include "ta_roienc/logging.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace ta_roienc {

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
        // No sink: stderr, one line, no level prefix on warnings -- what a person running a
        // command-line tool should see. Trace and Debug are dropped rather than printed,
        // because the built-in sink has no way to be turned down.
        if (level == LogLevel::Trace || level == LogLevel::Debug) {
            return;
        }
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }

    void debug(std::string_view m) const { write(LogLevel::Debug, m); }
    void info(std::string_view m) const { write(LogLevel::Info, m); }
    void warn(std::string_view m) const { write(LogLevel::Warning, m); }
    void error(std::string_view m) const { write(LogLevel::Error, m); }

private:
    LogSink m_sink;
};

} // namespace ta_roienc
