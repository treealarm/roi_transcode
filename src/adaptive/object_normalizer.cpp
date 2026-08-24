#include "object_normalizer.h"

#include <algorithm>
#include <cmath>

namespace smartvideo {

namespace {

int clamp_i(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

void clamp_rect(Rect& r, int width, int height)
{
    r.left = clamp_i(r.left, 0, width);
    r.right = clamp_i(r.right, 0, width);
    r.top = clamp_i(r.top, 0, height);
    r.bottom = clamp_i(r.bottom, 0, height);
}

bool rect_valid(const Rect& r)
{
    return r.right > r.left && r.bottom > r.top;
}

Detection to_detection(
    const Box& b, const Quality& quality, int width, int height)
{
    Detection d;
    d.x1 = static_cast<int>(std::lround(b.x * width));
    d.y1 = static_cast<int>(std::lround(b.y * height));
    d.x2 = static_cast<int>(std::lround((b.x + b.width) * width));
    d.y2 = static_cast<int>(std::lround((b.y + b.height) * height));

    // An explicitly configured kind keeps its number. Somebody said what they wanted for
    // this class of thing, and a model second-guessing that would make the setting a
    // suggestion. Its importance is derived back from the delta only so that regions can
    // still be ordered against the weighed ones -- see build().
    const auto configured = quality.kinds.find(b.kind);
    if (configured != quality.kinds.end()) {
        const KindQuality& k = configured->second;
        d.qp_delta = k.qp_delta;
        d.pad_x = static_cast<int>(std::lround(k.pad * width));
        d.pad_y = static_cast<int>(std::lround(k.pad * height));
        d.importance = importance_from_qp(k.qp_delta, quality.importance);
    } else {
        // Nothing configured for this kind, which in ta_vms is every kind: the archive pass
        // sends no per-kind entries at all. §11 decides.
        const Object object = object_from_box(b, quality.importance);
        d.importance = importance_of(object, quality.importance);
        d.qp_delta = qp_from_importance(d.importance, quality.importance);
        const KindQuality defaults;
        d.pad_x = static_cast<int>(std::lround(defaults.pad * width));
        d.pad_y = static_cast<int>(std::lround(defaults.pad * height));
    }
    // Half the region's own delta, which is what softens the step at its edge.
    d.pad_qp_delta = d.qp_delta / 2;
    return d;
}

} // namespace

Object object_from_box(const Box& box, const ImportancePolicy& policy)
{
    Object o;
    o.track_id = box.track_id;
    o.kind = box.kind;
    o.confidence = box.confidence > 0.0f ? box.confidence : policy.assumed_confidence;
    o.geometry = BoundingBox{box.x, box.y, box.width, box.height};
    o.semantic_priority = default_semantic_priority(box.kind, policy.unknown_priority);
    return o;
}

std::vector<Rect> RoiMapBuilder::build(int width, int height,
    const std::vector<Detection>& detections, int background_qp_delta) const
{
    std::vector<Rect> out;
    if (width <= 0 || height <= 0) {
        return out;
    }

    // §12: on overlap the more important region wins -- a plate inside a vehicle keeps the
    // plate's quality, a face inside a person keeps the face's.
    //
    // That is what this ordering *is*. The encoders resolve an overlap by taking the first
    // region that covers a block (Intel VPL and libavcodec's QSV wrapper both do), so
    // sorting by importance descending makes their rule and §12's the same rule. No
    // overlap test appears anywhere, and none is needed: it falls out of the order.
    std::vector<const Detection*> ordered;
    ordered.reserve(detections.size());
    for (const auto& d : detections) {
        ordered.push_back(&d);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const Detection* a, const Detection* b) { return a->importance > b->importance; });

    // Every object before any halo, and that ordering is load-bearing rather than tidy.
    //
    // Emitting each object with its own halo interleaved puts a nested object's halo ahead
    // of its parent's body: a face inside a person would surround itself with its halo at
    // roughly -4 and win over the person's own -7, so the torso beside the head would come
    // out worse than the rest of the person. A halo exists to soften the step *outside* a
    // region, never to cut into one.
    for (const Detection* dp : ordered) {
        Rect obj{
            .top = dp->y1,
            .bottom = dp->y2,
            .left = dp->x1,
            .right = dp->x2,
            .qp_delta = dp->qp_delta,
        };
        clamp_rect(obj, width, height);
        if (rect_valid(obj)) {
            out.push_back(obj);
        }
    }

    for (const Detection* dp : ordered) {
        Rect pad{
            .top = dp->y1 - dp->pad_y,
            .bottom = dp->y2 + dp->pad_y,
            .left = dp->x1 - dp->pad_x,
            .right = dp->x2 + dp->pad_x,
            .qp_delta = dp->pad_qp_delta,
        };
        clamp_rect(pad, width, height);
        if (rect_valid(pad)) {
            out.push_back(pad);
        }
    }

    if (background_qp_delta != 0) {
        out.push_back(Rect{
            .top = 0,
            .bottom = height,
            .left = 0,
            .right = width,
            .qp_delta = background_qp_delta,
        });
    }

    return out;
}

std::vector<Detection> RoiMapBuilder::detections_at(
    const std::vector<Box>& boxes, const Quality& quality, int width,
    int height, double t_sec) const
{
    std::vector<Detection> out;
    const double hold = quality.hold_sec > 0 ? quality.hold_sec : 0.25;

    for (const auto& b : boxes) {
        if (std::abs(b.t_sec - t_sec) > hold) {
            continue;
        }
        out.push_back(to_detection(b, quality, width, height));
    }
    return out;
}

std::vector<Detection> RoiMapBuilder::detections_from(
    const std::vector<Box>& boxes, const Quality& quality, int width,
    int height) const
{
    std::vector<Detection> out;
    out.reserve(boxes.size());
    for (const auto& b : boxes) {
        out.push_back(to_detection(b, quality, width, height));
    }
    return out;
}

} // namespace smartvideo
