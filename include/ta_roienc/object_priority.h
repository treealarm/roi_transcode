#pragma once

// Object priority ladder.
//
// Five named steps with a fixed QP delta for each. The public contract is this enum; how a
// detector class lands on a step is our business (see priority_from_importance). Desired
// deltas on a box still win when set.

#include <cstdint>

namespace ta_roienc {

enum class ObjectPriority {
    Background = 0,
    Low = 1,
    Normal = 2,
    High = 3,
    Critical = 4,
};

/// Canonical QP delta for a priority step. Background is neutral (0); the frame-wide
/// background penalty lives on Quality::background_qp_delta, not on an object.
inline int32_t qp_delta_for(ObjectPriority p)
{
    switch (p) {
        case ObjectPriority::Critical:   return -8;
        case ObjectPriority::High:       return -6;
        case ObjectPriority::Normal:     return -4;
        case ObjectPriority::Low:        return -2;
        case ObjectPriority::Background: return 0;
    }
    return 0;
}

inline const char* object_priority_name(ObjectPriority p)
{
    switch (p) {
        case ObjectPriority::Critical:   return "critical";
        case ObjectPriority::High:       return "high";
        case ObjectPriority::Normal:     return "normal";
        case ObjectPriority::Low:        return "low";
        case ObjectPriority::Background: return "background";
    }
    return "background";
}

/// Map an importance in [0, 1] onto the ladder. Thresholds follow the semantic table:
/// plate/face (>=0.95) Critical, person (>=0.80) High, vehicle (>=0.60) Normal,
/// weak/unknown Low, near-background Background.
ObjectPriority priority_from_importance(float importance);

/// Ordering key: Critical outranks High, and so on. Used wherever an overlap is resolved.
inline int32_t priority_rank(ObjectPriority p)
{
    return static_cast<int32_t>(p);
}

} // namespace ta_roienc
