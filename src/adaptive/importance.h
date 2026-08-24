#pragma once

// §11's model, evaluated. The policy itself is public (smartvideo/importance.h); this is
// the arithmetic that reads it.

#include "smartvideo/importance.h"
#include "smartvideo/object.h"

#include <cstdint>

namespace smartvideo {

/// The five-factor product of §11, clamped to [0, 1]. Two of the five are constant 1.0
/// until §13 gives them meaning, and the code says so where it happens.
float importance_of(const Object& object, const ImportancePolicy& policy);

/// Importance to a QP delta, along the line the specification's own numbers define.
int32_t qp_from_importance(float importance, const ImportancePolicy& policy);

/// The same line read backwards, for a region whose delta was configured by hand rather
/// than weighed. It exists so those regions can be ordered against the weighed ones on one
/// scale -- §12 resolves an overlap by importance, and that is only meaningful if every
/// region has one. Not a claim about what the operator meant: it is the importance their
/// number corresponds to, which is exactly what the ordering needs.
float importance_from_qp(int32_t qp_delta, const ImportancePolicy& policy);

} // namespace smartvideo
