// The spellings of FallbackPolicy.

#include "ta_roienc/policy.h"

namespace ta_roienc {

const char* fallback_policy_name(FallbackPolicy p)
{
    switch (p) {
    case FallbackPolicy::Permissive: return "permissive";
    case FallbackPolicy::Strict:     return "strict";
    }
    return "?";
}

} // namespace ta_roienc
