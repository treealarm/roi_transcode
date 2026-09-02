#include "sve/logging.h"

namespace sve {

const char* log_level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:   return "trace";
    case LogLevel::Debug:   return "debug";
    case LogLevel::Info:    return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error:   return "error";
    }
    return "?";
}

} // namespace sve
