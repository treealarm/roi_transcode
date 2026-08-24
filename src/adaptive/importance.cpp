// The importance model of TZ v2.0 §11.

#include "importance.h"

#include <algorithm>
#include <cmath>

namespace smartvideo {

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
    if (kind == "license_plate") return 1.00f;
    if (kind == "face")          return 0.95f;
    if (kind == "person")        return 0.80f;
    if (kind == "vehicle")       return 0.60f;
    if (kind == "animal")        return 0.60f;
    if (kind == "road")          return 0.10f;
    if (kind == "background")    return 0.05f;
    return unknown;
}

namespace {

float lookup(const std::unordered_map<std::string, float>& table, const std::string& key,
    float fallback)
{
    const auto it = table.find(key);
    return it != table.end() ? it->second : fallback;
}

} // namespace

float importance_of(const Object& object, const ImportancePolicy& policy)
{
    const float semantic = lookup(policy.semantic_priority, object.kind,
        default_semantic_priority(object.kind, policy.unknown_priority));
    const float task = lookup(policy.task_priority, object.kind, 1.0f);

    // Silence is not zero. A detector that reports no confidence -- most ONVIF cameras --
    // would otherwise have every one of its regions multiplied away to nothing.
    const float confidence =
        object.confidence > 0.0f ? object.confidence : policy.assumed_confidence;

    // §13 and §15 territory, and honestly constant until they exist. Written out rather
    // than dropped from the product so the shape of §11 stays visible: when temporal
    // stability arrives it replaces a 1.0f here and nothing else moves.
    const float temporal_stability = 1.0f;
    const float motion_weight = 1.0f;

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
