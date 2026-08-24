#pragma once

// How much a region is worth -- TZ v2.0 §11.
//
// The specification gives the model as a product of five factors with a result in [0, 1]:
//
//     Importance = SemanticPriority x TaskPriority x Confidence
//                  x TemporalStability x MotionWeight
//
// and a table of semantic priorities: background 0.05, road 0.10, vehicle 0.60, pedestrian
// 0.80, face 0.95, licence plate 1.00. Those numbers are the defaults below.
//
// What §11 does not say is how importance becomes a QP delta. §12 does, without meaning to:
// its worked example gives a vehicle -4 and a plate -10, and §11 gives those two an
// importance of 0.60 and 1.00. Exactly one line passes through two points --
//
//     qp_delta = 5 - 15 * importance
//
// -- and it puts the background of §11 (importance 0.05) at +4.25, which is a sensible
// penalty rather than an accident. So the defaults here are read off the specification
// rather than invented, which is worth writing down: they look arbitrary otherwise.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace smartvideo {

struct ImportancePolicy {
    /// Semantic priority per kind, 0..1 -- how much this class of thing is worth in
    /// general. A kind named here overrides the built-in table of §11; a kind in neither
    /// gets `unknown_priority`.
    std::unordered_map<std::string, float> semantic_priority;

    /// What the task at hand cares about, multiplied on top of the class. A plate matters
    /// more to number-plate recognition than to counting pedestrians, and §11 keeps the two
    /// apart for exactly that reason. Anything not named here is 1.
    std::unordered_map<std::string, float> task_priority;

    /// What a box whose class nobody reported is worth. Not zero: a camera whose firmware
    /// omits tt:Class reports every object as "unknown", and treating those as worthless
    /// would spend the whole frame on background. Halfway is the honest answer to "something
    /// is there and we do not know what".
    float unknown_priority = 0.5f;

    /// Confidence to assume when the detector reported none. §11 multiplies confidence in,
    /// so a missing value has to become something -- and silence means "did not say", not
    /// "not sure". Reading it as 0 would drop every region from a detector that reports no
    /// confidence at all, which is most ONVIF cameras.
    float assumed_confidence = 1.0f;

    /// The two ends of the importance-to-QP line. Defaults derived from §11 and §12 above.
    int32_t qp_at_zero = 5;
    int32_t qp_at_one = -10;
};

/// The §11 table, for a kind this deployment has not overridden. Returns `unknown` for a
/// class the table does not name.
float default_semantic_priority(const std::string& kind, float unknown = 0.5f);

} // namespace smartvideo
