#include "ta_roienc/log.h"

namespace ta_roienc {

const char* verbosity_name(Verbosity level)
{
    switch (level) {
    case Verbosity::Trace:   return "trace";
    case Verbosity::Debug:   return "debug";
    case Verbosity::Info:    return "info";
    case Verbosity::Warning: return "warning";
    case Verbosity::Error:   return "error";
    }
    return "?";
}

} // namespace ta_roienc
