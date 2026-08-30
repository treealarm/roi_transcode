#pragma once

// §11's model, evaluated. The policy itself is public (sve/importance.h); this is
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

/// The five-factor product of §11 v2.0, clamped to [0, 1]. Still used internally to pick a
/// v3 ObjectPriority step (see priority_from_importance).
float importance_of(const Object& object, const ImportancePolicy& policy,
    const TrackEvidence& track = {});

/// Importance to a QP delta along the v2.0 line. Prefer qp_delta_for(ObjectPriority) for
/// the v3 public contract.
int32_t qp_from_importance(float importance, const ImportancePolicy& policy);

float importance_from_qp(int32_t qp_delta, const ImportancePolicy& policy);

} // namespace sve
