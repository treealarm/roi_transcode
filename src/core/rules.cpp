// The spellings of EngineFallback.

#include "ta_roienc/rules.h"

namespace ta_roienc {

const char* engine_fallback_name(EngineFallback p)
{
    switch (p) {
    case EngineFallback::Permissive: return "permissive";
    case EngineFallback::Strict:     return "strict";
    }
    return "?";
}

} // namespace ta_roienc
