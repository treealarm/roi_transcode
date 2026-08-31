#include "importance_map.h"
#include "object_normalizer.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace sve;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

/// AT-004 -- ROI normalization edge cases, TZ v4.1 §48.
///
/// "Ожидаемый результат deterministic", and the point of writing it down is that every one
/// of these inputs is a thing a real detector produces. A box that runs off the left edge
/// because the object is half out of frame, a zero-area box from a tracker that lost its
/// target, two boxes on the same car. What must never happen is a rectangle the encoder
/// cannot use: negative coordinates, an edge past the frame, or left >= right.
void at_004_normalization_edge_cases()
{
    RoiMapBuilder builder;
    Quality q;
    q.background_qp_delta = 6;

    const auto usable = [](const Rect& r, int w, int h) {
        return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h
            && r.left < r.right && r.top < r.bottom;
    };

    // Negative, and outside the frame on two sides at once. Clamped, not dropped: half a
    // plate is still the half worth protecting.
    Detection off_left;
    off_left.x1 = -200;
    off_left.y1 = -50;
    off_left.x2 = 100;
    off_left.y2 = 80;
    off_left.qp_delta = -10;
    off_left.pad_x = 40;
    off_left.pad_y = 40;
    for (const Rect& r : builder.build(640, 480, {off_left}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Entirely outside. Nothing to protect, so the object contributes no rectangle -- but
    // the background still does, and the encoder still gets a usable map.
    Detection gone;
    gone.x1 = 700;
    gone.y1 = 500;
    gone.x2 = 800;
    gone.y2 = 560;
    gone.qp_delta = -10;
    const auto outside = builder.build(640, 480, {gone}, q);
    for (const Rect& r : outside) {
        CHECK(usable(r, 640, 480));
        CHECK(r.qp_delta == q.background_qp_delta);
    }

    // Zero area, which is what a tracker emits on the frame it loses the object. It must
    // not become a one-pixel-wide rectangle either: left < right is the invariant.
    Detection empty;
    empty.x1 = 300;
    empty.y1 = 200;
    empty.x2 = 300;
    empty.y2 = 200;
    empty.qp_delta = -10;
    for (const Rect& r : builder.build(640, 480, {empty}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Tiny: smaller than a macroblock. Whatever the geometry does with it, the result is
    // still a rectangle an encoder can take.
    Detection tiny;
    tiny.x1 = 320;
    tiny.y1 = 240;
    tiny.x2 = 322;
    tiny.y2 = 241;
    tiny.qp_delta = -10;
    for (const Rect& r : builder.build(640, 480, {tiny}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Overlap. Both survive, and the stronger one comes first: §11 resolves an overlap by
    // index, so the order is the resolution and getting it backwards would protect the
    // less important object.
    Detection strong;
    strong.x1 = 100;
    strong.y1 = 100;
    strong.x2 = 300;
    strong.y2 = 200;
    strong.qp_delta = -12;
    Detection weak = strong;
    weak.x1 = 200;
    weak.x2 = 400;
    weak.qp_delta = -4;
    const auto pair = builder.build(640, 480, {strong, weak}, q);
    CHECK(pair.size() >= 2);
    for (const Rect& r : pair) {
        CHECK(usable(r, 640, 480));
    }
    CHECK(pair[0].qp_delta <= pair[1].qp_delta);

    // Deterministic, which §48 asks for in as many words: the same input twice is the same
    // output twice, down to the rectangle order.
    const auto again = builder.build(640, 480, {strong, weak}, q);
    CHECK(again.size() == pair.size());
    for (std::size_t i = 0; i < again.size(); ++i) {
        CHECK(again[i].left == pair[i].left);
        CHECK(again[i].top == pair[i].top);
        CHECK(again[i].right == pair[i].right);
        CHECK(again[i].bottom == pair[i].bottom);
        CHECK(again[i].qp_delta == pair[i].qp_delta);
    }
}

} // namespace

int main()
{
    at_004_normalization_edge_cases();

    RoiMapBuilder builder;

    Detection plate;
    plate.x1 = 800;
    plate.y1 = 500;
    plate.x2 = 960;
    plate.y2 = 560;
    plate.qp_delta = -10;
    plate.pad_qp_delta = -5;
    plate.pad_x = 40;
    plate.pad_y = 22;

    Quality background_only;
    background_only.background_qp_delta = 6;

    const auto rects = builder.build(1920, 1080, {plate}, background_only);
    CHECK(rects.size() == 3);
    CHECK(rects[0].qp_delta == -10);
    CHECK(rects[0].left == 800);
    CHECK(rects[0].right == 960);
    CHECK(rects[1].qp_delta == -5);
    CHECK(rects[1].left == 760);
    CHECK(rects[1].right == 1000);
    CHECK(rects[2].qp_delta == 6);
    CHECK(rects[2].left == 0);
    CHECK(rects[2].right == 1920);
    CHECK(rects[2].bottom == 1080);

    Quality quality;
    quality.hold_sec = 0.25;
    // Keep the historical pad-as-halo semantics for this fixture; v3 FixedPixels+min
    // side would shift the AABB and is covered by v3_policy_test.
    quality.margin_mode = MarginMode::FractionOfFrame;
    quality.min_roi_side = 0;
    quality.kinds["plate"] = KindQuality{-10, 0.02};
    const std::vector<Box> boxes{Box{
        .t_sec = 1.25,
        .kind = "plate",
        .x = 0.5f,
        .y = 0.5f,
        .width = 0.1f,
        .height = 0.05f,
    }};

    const auto at = builder.detections_at(boxes, quality, 1000, 1000, 1.20);
    CHECK(at.size() == 1);
    CHECK(at[0].x1 == 500);
    CHECK(at[0].x2 == 600);
    CHECK(at[0].qp_delta == -10);

    const auto miss = builder.detections_at(boxes, quality, 1000, 1000, 9.0);
    CHECK(miss.empty());

    // Without the time window every box resolves, whatever its timestamp.
    const auto all = builder.detections_from(boxes, quality, 1000, 1000);
    CHECK(all.size() == 1);
    CHECK(all[0].x1 == 500);

    QpDeltaMapBuilder qp_map;
    const auto grid = qp_map.build(1920, 1080, 32, rects);
    CHECK(grid.block == 32);
    CHECK(grid.cols == 60);
    CHECK(grid.rows == 34);
    CHECK(grid.deltas.size() == 60 * 34);
    // Object at [800,960)×[500,560) wins over pad and background.
    CHECK(grid.deltas[(500 / 32) * 60 + (800 / 32)] == -10);
    CHECK(grid.deltas[(500 / 32) * 60 + (760 / 32)] == -5);
    CHECK(grid.deltas[0] == 6);

    // A triangle over the bottom-left half: the AABB would protect the top-right corner of
    // that box, the polygon must not. Build through detections_from so the shaped path runs.
    Quality poly_q;
    poly_q.background_qp_delta = 6;
    poly_q.pad = 0;
    poly_q.margin_mode = MarginMode::FractionOfFrame;
    poly_q.min_roi_side = 0;
    poly_q.max_protected_fraction = 0; // no area budget for this check
    poly_q.kinds["vehicle"] = KindQuality{-8, 0.0};
    Box tri;
    tri.kind = "vehicle";
    tri.x = 0;
    tri.y = 0;
    tri.width = 1;
    tri.height = 1;
    tri.polygon = {{0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const auto poly_dets = builder.detections_from({tri}, poly_q, 64, 64);
    CHECK(poly_dets.size() == 1);
    CHECK(poly_dets[0].polygon.size() == 3);
    const auto poly_rects = builder.build(64, 64, poly_dets, poly_q);
    // Paint the rects onto a qp map and check the corners: bottom-left protected, top-right not.
    const auto poly_grid = qp_map.build(64, 64, 16, poly_rects);
    CHECK(poly_grid.cols == 4 && poly_grid.rows == 4);
    CHECK(poly_grid.deltas[3 * 4 + 0] == -8); // bottom-left block
    CHECK(poly_grid.deltas[0 * 4 + 3] == 6);  // top-right stays background

    // A naive resize drops the background (it is last). truncate_roi_rects keeps it and
    // spends the remaining slots on earlier objects.
    std::vector<sve::Rect> many;
    for (int32_t i = 0; i < 10; ++i) {
        many.push_back({.top = 0, .bottom = 10, .left = i * 10, .right = i * 10 + 8,
            .qp_delta = -5});
    }
    many.push_back({.top = 0, .bottom = 100, .left = 0, .right = 100, .qp_delta = 14});
    int32_t lost = 0;
    sve::truncate_roi_rects(many, 4, 100, 100, &lost);
    CHECK(many.size() == 4);
    CHECK(lost == 7); // 10 objects -> 3 kept + background; 7 objects dropped
    CHECK(many.back().qp_delta == 14);
    CHECK(many.back().right == 100 && many.back().bottom == 100);

    std::puts("roi_map_builder_test ok");
    return 0;
}
