// The spellings of the two policy enums -- §22's FallbackPolicy and §14's QualityPolicy.
// One file because one header declares both.

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

const char* quality_policy_name(QualityPolicy p)
{
    switch (p) {
    case QualityPolicy::OBJECT_FIRST:  return "OBJECT_FIRST";
    case QualityPolicy::BALANCED:      return "BALANCED";
    case QualityPolicy::BITRATE_FIRST: return "BITRATE_FIRST";
    }
    return "?";
}

} // namespace sve
