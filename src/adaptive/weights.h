#pragma once

// The importance model, evaluated. The policy itself is public (ta_roienc/weighting.h); this is
// the arithmetic that reads it.

#include "ta_roienc/weighting.h"
#include "ta_roienc/tracked_object.h"
#include "ta_roienc/salience.h"

#include <cstdint>

namespace ta_roienc {

/// TemporalStability ∈ [floor, 1]: rises with track age. 1.0 when there is no track.
float temporal_stability_of(const TrackHistory& track, const SalienceRules& policy);

/// MotionWeight ∈ [floor, 1] when enabled; otherwise 1.0. Speed is in frame-diagonals/sec.
float motion_weight_of(const TrackHistory& track, const SalienceRules& policy);

/// The five-factor importance product, clamped to [0, 1]. Used internally to pick an
/// Salience step (see salience_from_importance).
float importance_of(const Object& object, const SalienceRules& policy,
    const TrackHistory& track = {});

/// Importance to a QP delta along the importance line. Prefer
/// qp_offset_for(Salience), which is the public contract.
int32_t qp_from_importance(float importance, const SalienceRules& policy);

float importance_from_qp(int32_t qp_delta, const SalienceRules& policy);

} // namespace ta_roienc
