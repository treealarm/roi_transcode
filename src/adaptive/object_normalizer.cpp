#include "object_normalizer.h"

#include "importance.h"
#include "ta_roienc/geometry.h"
#include "ta_roienc/object_priority.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <span>
#include <vector>

namespace ta_roienc {

/// The region budget used when neither the caller nor the backend names one.
///
/// 256 because that is the largest number any backend here can carry -- mfxExtEncoderROI's
/// ROI[] is a fixed array of that size, and no other backend takes fewer *by structure*
/// (the drivers take far fewer, and say so through EncoderCapabilities::max_roi_regions).
/// So it is a ceiling that never truncates anything a backend could have used, which is
/// what "no budget was given" should mean.
constexpr int32_t kDefaultRegionBudget = 256;

namespace {

/// How much of the frame the area budget allows to be protected, in pixels.
///
/// Written once because it is read twice -- the AABB builder and the block-map builder --
/// and a meaningful number computed in two places is where one copy changes and the other
/// does not.
///
/// **A fraction of zero means nothing may be protected.** No sentinel is needed for "no
/// cap": that is already spelled 1.0, because no region can exceed the frame it lies in.
double protected_area_budget(const Quality& quality, double frame_area)
{
    return std::max(0.0f, quality.max_protected_fraction) * frame_area;
}


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

/// Halo size in pixels for one axis.
int32_t halo_px(const Quality& quality, int32_t frame_extent, double kind_pad)
{
    if (kind_pad > 0.0) {
        return static_cast<int32_t>(std::lround(kind_pad * frame_extent));
    }
    if (quality.margin_mode == MarginMode::FixedPixels) {
        return std::max(quality.margin_px, 0);
    }
    return static_cast<int32_t>(std::lround(quality.pad * frame_extent));
}

/// Expand by fixed-pixel margin when that mode is on, then to at least min_roi_side.
/// FractionOfFrame keeps the AABB as the body, and pad is only the halo.
/// KindQuality.pad / Quality.pad still feed Detection::pad_* for the soft rim — they never
/// double as the body margin, so an explicit pad_norm does not inflate the box twice.
void apply_geometry_policy(Detection& d, int32_t width, int32_t height, const Quality& quality)
{
    if (quality.margin_mode == MarginMode::FixedPixels) {
        const int32_t m = std::max(quality.margin_px, 0);
        d.x1 -= m;
        d.x2 += m;
        d.y1 -= m;
        d.y2 += m;
    }

    const int32_t min_side = std::max(quality.min_roi_side, 0);
    if (min_side > 0) {
        int32_t w = d.x2 - d.x1;
        int32_t h = d.y2 - d.y1;
        if (w < min_side && w > 0) {
            const int32_t grow = min_side - w;
            d.x1 -= grow / 2;
            d.x2 += grow - grow / 2;
        }
        if (h < min_side && h > 0) {
            const int32_t grow = min_side - h;
            d.y1 -= grow / 2;
            d.y2 += grow - grow / 2;
        }
    }

    d.x1 = clamp_i(d.x1, 0, width);
    d.x2 = clamp_i(d.x2, 0, width);
    d.y1 = clamp_i(d.y1, 0, height);
    d.y2 = clamp_i(d.y2, 0, height);
}

/// Whether every number this box carries is one arithmetic can use.
///
/// The public API takes floats from whoever fills a Box -- a detector, a config file, a
/// gRPC message -- and nothing upstream promises they are finite. A NaN reaches three
/// places that cannot survive it: std::lround, whose result on a NaN is undefined; the
/// comparator of a stable_sort, where a NaN breaks the strict weak ordering the standard
/// requires and takes the sort with it; and the polygon rasteriser, where floor(NaN) is
/// converted to int. An infinity is the same story through the multiplication by the frame
/// size.
///
/// So a box that carries one is dropped here, at the boundary, in the same way degenerate
/// boxes are: the frame still encodes, and one region is missing rather than the process.
/// Checked once per box on the way in rather than at each of the three places, because the
/// three are far apart and the next one added would not know to look.
bool box_is_finite(const Box& b)
{
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(b.x) || !finite(b.y) || !finite(b.width) || !finite(b.height)
        || !finite(b.confidence)) {
        return false;
    }
    return std::ranges::all_of(b.polygon, [&](const Point& p) {
        return finite(p.x) && finite(p.y);
    });
}

Detection to_detection(
    const Box& b, const Quality& quality, int width, int height)
{
    Detection d;
    d.track_id = b.track_id;
    // Prefer the polygon's own extent when one is present: a caller that fills only the
    // outline should still get a coherent AABB for the area budget and for overlays.
    if (b.polygon.size() >= 3) {
        d.polygon = b.polygon;
        const BoundingBox bb = bounding_box_of(Polygon{b.polygon});
        d.x1 = static_cast<int>(std::lround(bb.x * width));
        d.y1 = static_cast<int>(std::lround(bb.y * height));
        d.x2 = static_cast<int>(std::lround((bb.x + bb.width) * width));
        d.y2 = static_cast<int>(std::lround((bb.y + bb.height) * height));
    } else {
        d.x1 = static_cast<int>(std::lround(b.x * width));
        d.y1 = static_cast<int>(std::lround(b.y * height));
        d.x2 = static_cast<int>(std::lround((b.x + b.width) * width));
        d.y2 = static_cast<int>(std::lround((b.y + b.height) * height));
    }

    // An explicitly configured kind keeps its number. Somebody said what they wanted for
    // this class of thing, and a model second-guessing that would make the setting a
    // suggestion. Its importance is derived back from the delta only so that regions can
    // still be ordered against the weighed ones -- see build().
    const auto configured = quality.kinds.find(b.kind);
    if (configured != quality.kinds.end()) {
        const KindQuality& k = configured->second;
        d.qp_delta = k.qp_delta;
        d.pad_x = halo_px(quality, width, k.pad);
        d.pad_y = halo_px(quality, height, k.pad);
        d.importance = importance_from_qp(k.qp_delta, quality.importance);
        d.priority = priority_from_importance(d.importance);
    } else {
        // Nothing configured for this kind.
        // Order: desired_qp_delta → explicit priority → kind/confidence → ladder (or v2 line).
        const Object object = object_from_box(b, quality.importance);
        d.importance = importance_of(object, quality.importance, b.track);
        if (b.has_priority) {
            d.priority = b.priority;
        } else {
            d.priority = priority_from_importance(d.importance);
        }
        // The ladder is the contract; continuous importance stays the way a step is chosen,
        // not a second way to answer. A caller with a number of its own says so through
        // desired_qp_delta.
        d.qp_delta = b.has_desired_qp_delta ? b.desired_qp_delta : qp_delta_for(d.priority);
        if (b.has_priority || b.has_desired_qp_delta) {
            // Ordering against other regions still needs a comparable importance.
            d.importance = importance_from_qp(d.qp_delta, quality.importance);
        }
        d.pad_x = halo_px(quality, width, 0.0);
        d.pad_y = halo_px(quality, height, 0.0);
    }

    apply_geometry_policy(d, width, height, quality);
    d.pad_qp_delta = d.qp_delta / 2;
    return d;
}

void paint_mask_on_qp_map(QpDeltaMap& map, const uint8_t* mask, int width, int height,
    int8_t qp)
{
    if (map.cols <= 0 || map.rows <= 0 || !mask || width <= 0 || height <= 0) {
        return;
    }
    for (int by = 0; by < map.rows; ++by) {
        const int fy0 = by * map.block;
        const int fy1 = std::min(fy0 + map.block, map.height);
        for (int bx = 0; bx < map.cols; ++bx) {
            const int fx0 = bx * map.block;
            const int fx1 = std::min(fx0 + map.block, map.width);
            bool hit = false;
            for (int y = fy0; y < fy1 && !hit; ++y) {
                const int my = y * height / map.height;
                const uint8_t* row = mask + static_cast<size_t>(my) * width;
                for (int x = fx0; x < fx1; ++x) {
                    const int mx = x * width / map.width;
                    if (row[mx] != 0) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                map.deltas[static_cast<size_t>(by) * map.cols + bx] = qp;
            }
        }
    }
}

void paint_aabb_on_qp_map(QpDeltaMap& map, const Rect& r)
{
    if (!rect_valid(r) || map.block <= 0) {
        return;
    }
    const int8_t qp = static_cast<int8_t>(std::clamp(r.qp_delta, kMinQpDelta, kMaxQpDelta));
    const int c0 = std::clamp(r.left / map.block, 0, map.cols - 1);
    const int c1 = std::clamp((r.right - 1) / map.block, 0, map.cols - 1);
    const int r0 = std::clamp(r.top / map.block, 0, map.rows - 1);
    const int r1 = std::clamp((r.bottom - 1) / map.block, 0, map.rows - 1);
    for (int y = r0; y <= r1; ++y) {
        for (int x = c0; x <= c1; ++x) {
            map.deltas[static_cast<size_t>(y * map.cols + x)] = qp;
        }
    }
}

/// Shape-preserving path: every detection is painted onto the block qp-map (polygons by
/// rasterisation, AABBs by their rectangle), weak first so the more important overwrite,
/// then merged back into encoder rectangles. Same representation a mask uses.
///
/// `coding_block` is the qp_map cell size. `map_region_limit` 0 means keep every merged
/// rectangle (qp_map backends); otherwise strongest-first truncate like rectangle APIs.
/// The detection's body as a rectangle inside the frame, or nothing when it does not
/// survive the clamp.
///
/// One definition because three places want it: the two builders' walks and the affordable
/// set below. They were three copies of the same designated initialiser plus clamp_rect
/// plus rect_valid, which is the shape of a rounding rule or a clamp bound changing in one
/// of them and not the others.
std::optional<Rect> body_rect(const Detection& d, int width, int height)
{
    Rect r{
        .top = d.y1,
        .bottom = d.y2,
        .left = d.x1,
        .right = d.x2,
        .qp_delta = d.qp_delta,
    };
    clamp_rect(r, width, height);
    if (!rect_valid(r)) {
        return std::nullopt;
    }
    return r;
}

/// Which detections the area budget can pay for.
///
/// One policy, called by both builders, so that the walk that decides *who* gets the
/// budget exists in one place.
///
/// `detections` is walked in the order given and the caller owes it descending importance:
/// the budget is spent on the critical objects first, and a plate has to be able to outbid
/// the wall behind it. Areas are summed rather than united, so a face inside a person spends
/// its own area again -- that errs towards spending less, which is the safe direction.
std::unordered_set<const Detection*> affordable_detections(
    std::span<const Detection* const> by_importance_desc, const Quality& quality,
    int width, int height)
{
    const double budget = protected_area_budget(
        quality, static_cast<double>(width) * static_cast<double>(height));
    double spent = 0.0;

    std::unordered_set<const Detection*> paid;
    paid.reserve(by_importance_desc.size());
    for (const Detection* dp : by_importance_desc) {
        const std::optional<Rect> body = body_rect(*dp, width, height);
        if (!body) {
            continue;
        }
        const double area = static_cast<double>(body->right - body->left)
            * static_cast<double>(body->bottom - body->top);
        if (spent + area > budget) {
            continue;
        }
        spent += area;
        paid.insert(dp);
    }
    return paid;
}

std::vector<Rect> build_shaped(int width, int height,
    const std::vector<Detection>& detections, const Quality& quality,
    int32_t coding_block, int32_t map_region_limit, int32_t* dropped)
{
    std::vector<Rect> empty;
    if (width <= 0 || height <= 0) {
        return empty;
    }

    std::vector<const Detection*> ordered;
    ordered.reserve(detections.size());
    for (const auto& d : detections) {
        ordered.push_back(&d);
    }
    // Ascending: weak first, important last -- the map keeps the last write, matching the
    // encoder's "first region wins" once rects_from_qp_map sorts strongest-|delta| first.
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const Detection* a, const Detection* b) { return a->importance < b->importance; });

    const int block = coding_block > 0 ? coding_block : 16;
    QpDeltaMap map;
    map.block = block;
    map.width = width;
    map.height = height;
    map.cols = (width + block - 1) / block;
    map.rows = (height + block - 1) / block;
    const int8_t bg = static_cast<int8_t>(std::clamp(quality.background_qp_delta, kMinQpDelta, kMaxQpDelta));
    map.deltas.assign(static_cast<size_t>(map.cols) * map.rows, bg);

    std::vector<uint8_t> scratch(static_cast<size_t>(width) * height);

    struct Afforded {
        const Detection* dp = nullptr;
        int32_t body_delta = 0;
        int32_t pad_delta = 0;
    };
    std::vector<Afforded> afforded;
    afforded.reserve(ordered.size());

    // Two walks, and they go in opposite directions on purpose.
    //
    // The budget is spent on the critical objects first, so affordability is decided
    // walking importance *downwards* -- a plate must be able to outbid a wall behind it.
    // Painting goes the other way, upwards, because the map keeps the last write and the
    // important object has to be written last. Deciding and painting in one pass means one
    // of the two orders is wrong: a plate could be refused for a wall that had already
    // taken the room.
    //
    // `ordered` is ascending, so reading it backwards is the descending order the budget
    // wants -- one sort serves both walks, and equal importances are interchangeable for
    // painting because the map keeps the last write either way.
    std::vector<const Detection*> by_importance(ordered.rbegin(), ordered.rend());
    const std::unordered_set<const Detection*> within_budget =
        affordable_detections(by_importance, quality, width, height);

    for (const Detection* dp : ordered) {
        if (!body_rect(*dp, width, height)) {
            continue;
        }
        // What did not fit is painted at zero rather than left unpainted: the map starts
        // filled with the background penalty, so an object nobody paints is not neutral, it
        // is *degraded*. A region nobody could afford to sharpen is still not background.
        // Its halo does go -- a halo exists to soften a step that is no longer there.
        // RoiMapBuilder::build emits the unaffordable object with a delta of zero too.
        const bool paid = within_budget.contains(dp);
        afforded.push_back(Afforded{dp, paid ? dp->qp_delta : 0, paid ? dp->pad_qp_delta : 0});
    }

    // Every halo before every body, for the same reason as the AABB builder: a nested
    // object's halo must not cut into its parent's body.
    for (const Afforded& a : afforded) {
        if (a.pad_delta == 0) {
            continue;
        }
        const Detection* dp = a.dp;
        if (dp->polygon.size() >= 3 && (dp->pad_x > 0 || dp->pad_y > 0)) {
            std::memset(scratch.data(), 0, scratch.size());
            rasterize_polygon(Polygon{dp->polygon}, scratch.data(), width, height,
                static_cast<uint8_t>(kMaskMax));
            dilate_mask(scratch.data(), width, height, std::max(dp->pad_x, dp->pad_y));
            paint_mask_on_qp_map(map, scratch.data(), width, height,
                static_cast<int8_t>(std::clamp(a.pad_delta, kMinQpDelta, kMaxQpDelta)));
        } else if (dp->polygon.size() < 3) {
            Rect pad{
                .top = dp->y1 - dp->pad_y,
                .bottom = dp->y2 + dp->pad_y,
                .left = dp->x1 - dp->pad_x,
                .right = dp->x2 + dp->pad_x,
                .qp_delta = a.pad_delta,
            };
            clamp_rect(pad, width, height);
            paint_aabb_on_qp_map(map, pad);
        }
    }
    for (const Afforded& a : afforded) {
        // A body of zero is painted, not skipped. The map starts filled with the background
        // penalty, so skipping an object leaves it degraded rather than neutral -- the
        // opposite of what should happen to a region the budget could not pay for.
        const Detection* dp = a.dp;
        if (dp->polygon.size() >= 3) {
            std::memset(scratch.data(), 0, scratch.size());
            rasterize_polygon(Polygon{dp->polygon}, scratch.data(), width, height,
                static_cast<uint8_t>(kMaskMax));
            paint_mask_on_qp_map(map, scratch.data(), width, height,
                static_cast<int8_t>(std::clamp(a.body_delta, kMinQpDelta, kMaxQpDelta)));
        } else {
            Rect obj{
                .top = dp->y1,
                .bottom = dp->y2,
                .left = dp->x1,
                .right = dp->x2,
                .qp_delta = a.body_delta,
            };
            clamp_rect(obj, width, height);
            paint_aabb_on_qp_map(map, obj);
        }
    }

    const int32_t limit = map_region_limit >= 0
        ? map_region_limit
        : (quality.max_regions > 0 ? quality.max_regions : kDefaultRegionBudget);
    return rects_from_qp_map(map, limit, dropped);
}

} // namespace

Object object_from_box(const Box& box, const ImportancePolicy& policy)
{
    Object o;
    o.track_id = box.track_id;
    o.kind = box.kind;
    o.confidence = box.confidence > 0.0f ? box.confidence : policy.assumed_confidence;
    if (box.polygon.size() >= 3) {
        o.geometry = Polygon{box.polygon};
    } else {
        o.geometry = BoundingBox{box.x, box.y, box.width, box.height};
    }
    o.semantic_priority = default_semantic_priority(box.kind, policy.unknown_priority);
    return o;
}

std::vector<Rect> RoiMapBuilder::build(int width, int height,
    const std::vector<Detection>& detections, const Quality& quality,
    int32_t coding_block, int32_t map_region_limit, int32_t* dropped) const
{
    if (dropped) {
        *dropped = 0;
    }
    for (const auto& d : detections) {
        if (d.polygon.size() >= 3) {
            return build_shaped(width, height, detections, quality, coding_block,
                map_region_limit, dropped);
        }
    }

    std::vector<Rect> out;
    if (width <= 0 || height <= 0) {
        return out;
    }

    // On overlap the more important region wins -- a plate inside a vehicle keeps the
    // plate's quality, a face inside a person keeps the face's.
    //
    // That is what this ordering *is*. The encoders resolve an overlap by taking the first
    // region that covers a block (Intel VPL and libavcodec's QSV wrapper both do), so
    // sorting by importance descending makes their rule and ours the same rule. No overlap
    // test appears anywhere, and none is needed: it falls out of the order.
    std::vector<const Detection*> ordered;
    ordered.reserve(detections.size());
    for (const auto& d : detections) {
        ordered.push_back(&d);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const Detection* a, const Detection* b) { return a->importance > b->importance; });

    // The area budget, decided by the one function both builders call. Under a constant
    // quantizer there is no bitrate to hand around, but protection is still paid for: every
    // block held at a lower QP is paid for by the background it is taken from, and a frame
    // with no background left cannot pay.
    //
    // Areas are summed, not united, so nesting counts twice: a face inside a person spends
    // its own area again. That errs towards spending less, which is the safe direction --
    // and it is nearly free here, because the objects that nest are the small ones.
    //
    // What does not fit gets a delta of zero rather than being dropped. Dropped, it would
    // fall through to the background rectangle below and be *degraded*; a person nobody can
    // afford to sharpen is still not background. Its halo goes, though: a halo exists to
    // soften a step that is no longer there.
    const std::unordered_set<const Detection*> within_budget =
        affordable_detections(ordered, quality, width, height);

    // Every object before any halo, and that ordering is load-bearing rather than tidy.
    //
    // Emitting each object with its own halo interleaved puts a nested object's halo ahead
    // of its parent's body: a face inside a person would surround itself with its halo at
    // roughly -4 and win over the person's own -7, so the torso beside the head would come
    // out worse than the rest of the person. A halo exists to soften the step *outside* a
    // region, never to cut into one.
    std::vector<const Detection*> afforded;
    afforded.reserve(ordered.size());
    for (const Detection* dp : ordered) {
        std::optional<Rect> obj = body_rect(*dp, width, height);
        if (!obj) {
            continue;
        }
        // What did not fit keeps a delta of zero rather than being dropped: a region nobody
        // can afford to sharpen is still not background. Its halo goes with the money.
        if (within_budget.contains(dp)) {
            afforded.push_back(dp);
        } else {
            obj->qp_delta = 0;
        }
        out.push_back(*obj);
    }

    for (const Detection* dp : afforded) {
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

    if (quality.background_qp_delta != 0) {
        out.push_back(Rect{
            .top = 0,
            .bottom = height,
            .left = 0,
            .right = width,
            .qp_delta = quality.background_qp_delta,
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
        if (!box_is_finite(b)) {
            continue;
        }
        out.push_back(to_detection(b, quality, width, height));
    }
    return out;
}

void truncate_roi_rects(std::vector<Rect>& rects, int32_t budget, int32_t width, int32_t height,
    int32_t* dropped)
{
    if (budget <= 0 || static_cast<int32_t>(rects.size()) <= budget) {
        return;
    }

    std::optional<Rect> background;
    if (!rects.empty()) {
        const Rect& last = rects.back();
        if (last.left == 0 && last.top == 0 && last.right == width && last.bottom == height) {
            background = last;
            rects.pop_back();
        }
    }

    const int32_t object_slots = background ? std::max(budget - 1, 0) : budget;
    if (static_cast<int32_t>(rects.size()) > object_slots) {
        if (dropped) {
            *dropped += static_cast<int32_t>(rects.size()) - object_slots;
        }
        rects.resize(static_cast<size_t>(object_slots));
    }
    if (background) {
        rects.push_back(*background);
    }
}

} // namespace ta_roienc
