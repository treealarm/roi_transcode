// The spellings of FallbackPolicy.

#include "sve/policy.h"

namespace sve {

const char* fallback_policy_name(FallbackPolicy p)
{
    switch (p) {
    case FallbackPolicy::Permissive: return "permissive";
    case FallbackPolicy::Strict:     return "strict";
    }
    return "?";
}

} // namespace sve
