#pragma once

// The importance model, evaluated. The policy itself is public (sve/importance.h); this is
// the arithmetic that reads it.

#include "sve/importance.h"
#include "sve/object.h"
#include "sve/object_priority.h"

#include <cstdint>

namespace sve {

/// TemporalStability ∈ [floor, 1]: rises with track age. 1.0 when there is no track.
float temporal_stability_of(const TrackEvidence& track, const ImportancePolicy& policy);

/// MotionWeight ∈ [floor, 1] when enabled; otherwise 1.0. Speed is in frame-diagonals/sec.
float motion_weight_of(const TrackEvidence& track, const ImportancePolicy& policy);

/// The five-factor importance product, clamped to [0, 1]. Used internally to pick an
/// ObjectPriority step (see priority_from_importance).
float importance_of(const Object& object, const ImportancePolicy& policy,
    const TrackEvidence& track = {});

/// Importance to a QP delta along the importance line. Prefer
/// qp_delta_for(ObjectPriority), which is the public contract.
int32_t qp_from_importance(float importance, const ImportancePolicy& policy);

float importance_from_qp(int32_t qp_delta, const ImportancePolicy& policy);

} // namespace sve
