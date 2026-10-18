#pragma once

// The library's own end of the logging contract.
//
// One place that knows what to do when the application named no sink, so that the answer
// is the same everywhere and no caller writes to stderr directly. Held by value and
// cheap to copy: it is a std::function and a bool.

#include "ta_roienc/log.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace ta_roienc {

class Journal {
public:
    Journal() = default;
    explicit Journal(LogTarget sink) : m_sink(std::move(sink)) {}

    void write(Verbosity level, std::string_view message) const
    {
        if (m_sink) {
            m_sink(level, message);
            return;
        }
        // No sink: stderr, one line, no level prefix on warnings -- what a person running a
        // command-line tool should see. Trace and Debug are dropped rather than printed,
        // because the built-in sink has no way to be turned down.
        if (level == Verbosity::Trace || level == Verbosity::Debug) {
            return;
        }
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }

    void debug(std::string_view m) const { write(Verbosity::Debug, m); }
    void info(std::string_view m) const { write(Verbosity::Info, m); }
    void warn(std::string_view m) const { write(Verbosity::Warning, m); }
    void error(std::string_view m) const { write(Verbosity::Error, m); }

private:
    LogTarget m_sink;
};

} // namespace ta_roienc
