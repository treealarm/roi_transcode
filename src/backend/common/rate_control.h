#pragma once

// Turning RateControlPolicy::target_bitrate into what a software encoder's own rate control
// wants -- TZ §15 and §16.
//
// Here rather than twice, because libx264 and libx265 take the same three numbers under
// different names and the arithmetic between them is a decision, not a formatting detail: a
// copy that drifted would show up as one codec meeting §16 and the other not, which is
// exactly what this file was written after measuring.
//
// Header-only and dependency-free on purpose. The x264 and x265 components deliberately
// include nothing from backend/common except this kind of file -- backend_common.h pulls
// libavcodec in behind it, and §5.5 keeps those components clear of it.

#include "sve/policy.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace sve {

/// What a codec's own rate control is asked for, in the kilobits both libraries speak.
struct VbvPlan {
    /// Average target.
    int bitrate_kbps = 0;
    /// Ceiling on the buffer's fill rate.
    int max_kbps = 0;
    /// Buffer size, in kilobits. One second of `max_kbps`, which is what makes the ceiling
    /// a bound on the one-second window §16 measures.
    int buffer_kbits = 0;
};

/// bits per second as the encoders count them.
inline constexpr uint64_t kBitsPerKilobit = 1000;

/// The plan for `target_bitrate`, or a zeroed plan when no target was asked for.
///
/// **The ceiling is the target itself, and `max_peak_overshoot` is deliberately not spent
/// here.** Setting the ceiling to 120% of target -- reading the policy's allowance as the
/// setting -- is the obvious thing and it measures worse, because a VBV bound is not a hard
/// cap on every window: the encoder may drift within its buffer, so a ceiling placed at the
/// limit puts the measurement on or over it. Binding at the target leaves the allowance as
/// margin, which is what it is for.
///
/// Measured 2026-09-01 on 36 s of crowd footage, P95 of the 1-second rolling rate as a
/// percentage of target, ceiling at 120% against ceiling at 100%:
///
///     libx264, target 2 Mbps    99% -> 89%      target 6 Mbps   113% -> 100%
///     libx265, target 2 Mbps   127% -> 117%     target 6 Mbps   129% -> 116%
///
/// So x265 only meets §16's "P95 <= 120% target" the second way, and x264 meets it more
/// comfortably. The average moves the same direction on both, which settles it: 51% -> 76%
/// of target on x265, 60% -> 67% on x264.
constexpr VbvPlan vbv_plan_for(uint64_t target_bitrate)
{
    if (target_bitrate == 0) {
        return {};
    }
    const auto kbps = static_cast<int>(std::min<uint64_t>(
        target_bitrate / kBitsPerKilobit,
        static_cast<uint64_t>(std::numeric_limits<int>::max())));
    VbvPlan plan;
    plan.bitrate_kbps = std::max(kbps, 1);
    plan.max_kbps = plan.bitrate_kbps;
    plan.buffer_kbits = plan.max_kbps;
    return plan;
}

} // namespace sve
