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

    /// Confidence to assume when the detector reported none. Only reached for a class the
    /// table does not name -- see `weigh_confidence_on_known_classes`. Silence means "did
    /// not say", not "not sure": reading it as 0 would drop every region from a detector
    /// that reports no confidence at all, which is most ONVIF cameras.
    float assumed_confidence = 1.0f;

    /// Whether a class we recognise has its importance scaled by the detector's confidence.
    ///
    /// False, and that is a departure from the letter of §11, which multiplies confidence
    /// into every region. The reason is that the multiplication defeats the very ordering
    /// §11's own table sets up, and defeats it systematically rather than occasionally:
    ///
    ///     plate   semantic 1.00 x confidence 0.50 = 0.50
    ///     vehicle semantic 0.60 x confidence 0.95 = 0.57   <- the vehicle wins
    ///
    /// It is not hypothetical. Measured across this deployment's archive, a face averages
    /// 0.633 confidence against a person's 0.892, so 0.95 x 0.633 = 0.601 loses to
    /// 0.80 x 0.892 = 0.714 and a body outranks the face on it. Small objects are harder to
    /// detect confidently, and small objects -- faces, plates -- are exactly the ones with
    /// the highest semantic priority. The two are anti-correlated, so multiplying inverts
    /// the hierarchy as a rule.
    ///
    /// The point of adaptive encoding is that we know roughly what is in the frame and what
    /// it is worth: a plate outranks a car, a car outranks the background, a face outranks
    /// the body it belongs to. That ordering is the whole premise, not something to be
    /// decided per frame by how sure a detector happened to be.
    ///
    /// And confidence is already spent once. Detections arrive filtered
    /// (analytics_watch.min_confidence, 0.5 by default), so anything reaching here has
    /// already passed the "is it really there" test. Multiplying by it again applies the
    /// same signal twice. Confidence answers whether an object is present; the class
    /// answers what it is worth, and those are different questions.
    ///
    /// It still weighs on classes the table does not name -- for those, how sure the
    /// detector is really is all there is to go on. Set this true for the letter of §11.
    bool weigh_confidence_on_known_classes = false;

    /// The two ends of the importance-to-QP line.
    ///
    /// Defaults are the §12 worked example (vehicle 0.60 → −4, plate 1.00 → −10). The product
    /// operating point "plate −12, vehicle −6" is the same slope with ends at 3 and −12 -- set
    /// those on the wire rather than changing the library default, so a measurement against
    /// the specification still lands on the numbers the specification drew.
    int32_t qp_at_zero = 5;
    int32_t qp_at_one = -10;

    /// TemporalStability rises with how long a track has been alive. A brand-new track is
    /// worth `temporal_stability_floor`; after roughly `temporal_stability_tau_sec` seconds
    /// it has climbed most of the way to 1.0. Untracked boxes keep 1.0 -- punishing silence
    /// would drop every ONVIF region that has no track id.
    float temporal_stability_floor = 0.5f;
    float temporal_stability_tau_sec = 2.0f;

    /// MotionWeight is optional in §11. Off by default (factor stays 1.0). When on, a static
    /// track is worth `motion_weight_floor` and a track moving at `motion_speed_at_full`
    /// frame-diagonals per second reaches 1.0.
    bool use_motion_weight = false;
    float motion_weight_floor = 0.7f;
    float motion_speed_at_full = 0.15f;
};

/// What TemporalRoi knows about a track at one instant -- the two inputs §11 needs beyond
/// a single Object. Empty (`present == false`) leaves TemporalStability and MotionWeight at
/// 1.0, which is the right reading of "we have no history".
struct TrackEvidence {
    bool present = false;
    /// Seconds since the track's first sample, at the query time.
    float age_sec = 0.0f;
    /// Centre displacement in frame-diagonals per second. Zero when there is no pair of
    /// samples to measure against.
    float speed = 0.0f;
    int32_t sample_count = 0;
};

/// The §11 table, for a kind this deployment has not overridden. Returns `unknown` for a
/// class the table does not name.
float default_semantic_priority(const std::string& kind, float unknown = 0.5f);

/// Whether anything -- §11's table or this policy -- names this class. What separates "we
/// know what this is and what it is worth" from "something is there".
bool is_known_class(const std::string& kind, const ImportancePolicy& policy);

} // namespace smartvideo
