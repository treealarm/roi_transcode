// The importance model of TZ v2.0 §11.

#include "importance.h"

#include "smartvideo/object_priority.h"

#include <algorithm>
#include <cmath>

namespace smartvideo {

ObjectPriority priority_from_importance(float importance)
{
    const float i = std::clamp(importance, 0.0f, 1.0f);
    if (i >= 0.95f) {
        return ObjectPriority::Critical;
    }
    if (i >= 0.80f) {
        return ObjectPriority::High;
    }
    if (i >= 0.60f) {
        return ObjectPriority::Normal;
    }
    if (i >= 0.20f) {
        return ObjectPriority::Low;
    }
    return ObjectPriority::Background;
}

float default_semantic_priority(const std::string& kind, float unknown)
{
    // §11's own table, on the taxonomy ta_vms actually uses
    // (VmsAnalyticsSingleton/DetectionKinds.cs). Two notes on the mapping:
    //
    //   * §11 says "pedestrian" where our detector says "person" -- the same thing.
    //   * §11 has no animal. 0.60 puts it level with a vehicle: something real and worth
    //     keeping, not something the scene is about. Said here rather than left implicit,
    //     because it is our number and not the specification's.
    //
    // "road" and "background" are in §11's table but nothing reports them: they describe
    // what is left over, which is what background_qp_delta already handles.
    if (kind == "license_plate" || kind == "plate") return 1.00f;
    if (kind == "face")          return 0.95f;
    if (kind == "person")        return 0.80f;
    if (kind == "vehicle")       return 0.60f;
    if (kind == "animal")        return 0.60f;
    if (kind == "road")          return 0.10f;
    if (kind == "background")    return 0.05f;
    return unknown;
}

bool is_known_class(const std::string& kind, const ImportancePolicy& policy)
{
    if (policy.semantic_priority.find(kind) != policy.semantic_priority.end()) {
        return true;
    }
    // A sentinel the table cannot return for a class it names, so "not named" is
    // distinguishable from "named, and worth very little".
    constexpr float absent = -1.0f;
    return default_semantic_priority(kind, absent) != absent;
}

namespace {

float lookup(const std::unordered_map<std::string, float>& table, const std::string& key,
    float fallback)
{
    const auto it = table.find(key);
    return it != table.end() ? it->second : fallback;
}

} // namespace

float temporal_stability_of(const TrackEvidence& track, const ImportancePolicy& policy)
{
    if (!track.present) {
        // No history: leave the product alone. Punishing an untracked box would drop every
        // region from a source that never numbers its tracks.
        return 1.0f;
    }
    const float floor = std::clamp(policy.temporal_stability_floor, 0.0f, 1.0f);
    const float tau = std::max(policy.temporal_stability_tau_sec, 1e-3f);
    const float age = std::max(track.age_sec, 0.0f);
    // 1 - exp(-age/tau) climbs from 0 toward 1; floor is what a brand-new track is worth.
    return floor + (1.0f - floor) * (1.0f - std::exp(-age / tau));
}

float motion_weight_of(const TrackEvidence& track, const ImportancePolicy& policy)
{
    if (!policy.use_motion_weight || !track.present) {
        return 1.0f;
    }
    const float floor = std::clamp(policy.motion_weight_floor, 0.0f, 1.0f);
    const float full = std::max(policy.motion_speed_at_full, 1e-6f);
    const float t = std::clamp(track.speed / full, 0.0f, 1.0f);
    return floor + (1.0f - floor) * t;
}

float importance_of(const Object& object, const ImportancePolicy& policy,
    const TrackEvidence& track)
{
    const float semantic = lookup(policy.semantic_priority, object.kind,
        default_semantic_priority(object.kind, policy.unknown_priority));
    const float task = lookup(policy.task_priority, object.kind, 1.0f);

    // A class we recognise is worth what its class is worth, full stop.
    //
    // §11 multiplies confidence into everything, and that inverts its own table: small
    // objects are detected less confidently, and small objects are the ones with the
    // highest priority, so a body outranks the face on it. The reasoning is in
    // ImportancePolicy::weigh_confidence_on_known_classes; the short version is that
    // detections arrive already filtered by min_confidence, so "is it really there" has
    // been answered, and "what is it worth" is a different question with a different answer.
    //
    // For a class nobody named, confidence is all there is, so it weighs. Silence is not
    // zero there either -- most ONVIF cameras report no confidence at all, and reading that
    // as zero would multiply every one of their regions away.
    const bool known = is_known_class(object.kind, policy);
    const float confidence = (known && !policy.weigh_confidence_on_known_classes)
        ? 1.0f
        : (object.confidence > 0.0f ? object.confidence : policy.assumed_confidence);

    // The last two factors of §11. TemporalStability rises with how long the track has
    // persisted (proto's reading of the name). MotionWeight is optional and stays 1.0
    // until the policy turns it on -- §11 marks it that way.
    const float temporal_stability = temporal_stability_of(track, policy);
    const float motion_weight = motion_weight_of(track, policy);

    const float importance =
        semantic * task * confidence * temporal_stability * motion_weight;
    return std::clamp(importance, 0.0f, 1.0f);
}

float importance_from_qp(int32_t qp_delta, const ImportancePolicy& policy)
{
    const float span =
        static_cast<float>(policy.qp_at_one) - static_cast<float>(policy.qp_at_zero);
    if (std::fabs(span) < 1e-6f) {
        // A policy with both ends equal maps every importance to the same delta, so there
        // is nothing to read back. Middling is the least misleading answer.
        return 0.5f;
    }
    const float i = (static_cast<float>(qp_delta) - static_cast<float>(policy.qp_at_zero))
        / span;
    return std::clamp(i, 0.0f, 1.0f);
}

int32_t qp_from_importance(float importance, const ImportancePolicy& policy)
{
    const float i = std::clamp(importance, 0.0f, 1.0f);
    const float qp = static_cast<float>(policy.qp_at_zero)
        + (static_cast<float>(policy.qp_at_one) - static_cast<float>(policy.qp_at_zero)) * i;
    // The encoder's own range. Clamped rather than trusted: the endpoints are configurable,
    // and a caller who sets them wide should get a saturated answer, not a wrapped one.
    return std::clamp(static_cast<int32_t>(std::lround(qp)), -51, 51);
}

} // namespace smartvideo
