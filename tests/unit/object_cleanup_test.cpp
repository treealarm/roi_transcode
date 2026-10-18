#include "weight_grid.h"
#include "object_cleanup.h"

#include <cstddef>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ta_roienc;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

/// ROI normalization edge cases.
///
/// The result must be deterministic, and every one of these inputs is a thing a real
/// detector produces. A box that runs off the left edge because the object is half out of
/// frame, a zero-area box from a tracker that lost its target, two boxes on the same car.
/// What must never happen is a rectangle the encoder cannot use: negative coordinates, an
/// edge past the frame, or left >= right.
void normalization_edge_cases()
{
    RoiGridBuilder builder;
    QualityPlan q;
    q.background_qp_delta = 6;

    const auto usable = [](const Area& r, int w, int h) {
        return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h
            && r.left < r.right && r.top < r.bottom;
    };

    // Negative, and outside the frame on two sides at once. Clamped, not dropped: half a
    // plate is still the half worth protecting.
    Sighting off_left;
    off_left.x1 = -200;
    off_left.y1 = -50;
    off_left.x2 = 100;
    off_left.y2 = 80;
    off_left.qp_delta = -10;
    off_left.pad_x = 40;
    off_left.pad_y = 40;
    for (const Area& r : builder.build(640, 480, {off_left}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Entirely outside. Nothing to protect, so the object contributes no rectangle -- but
    // the background still does, and the encoder still gets a usable map.
    Sighting gone;
    gone.x1 = 700;
    gone.y1 = 500;
    gone.x2 = 800;
    gone.y2 = 560;
    gone.qp_delta = -10;
    const auto outside = builder.build(640, 480, {gone}, q);
    for (const Area& r : outside) {
        CHECK(usable(r, 640, 480));
        CHECK(r.qp_delta == q.background_qp_delta);
    }

    // Zero area, which is what a tracker emits on the frame it loses the object. It must
    // not become a one-pixel-wide rectangle either: left < right is the invariant.
    Sighting empty;
    empty.x1 = 300;
    empty.y1 = 200;
    empty.x2 = 300;
    empty.y2 = 200;
    empty.qp_delta = -10;
    for (const Area& r : builder.build(640, 480, {empty}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Tiny: smaller than a macroblock. Whatever the geometry does with it, the result is
    // still a rectangle an encoder can take.
    Sighting tiny;
    tiny.x1 = 320;
    tiny.y1 = 240;
    tiny.x2 = 322;
    tiny.y2 = 241;
    tiny.qp_delta = -10;
    for (const Area& r : builder.build(640, 480, {tiny}, q)) {
        CHECK(usable(r, 640, 480));
    }

    // Overlap. Both survive, and the stronger one comes first: overlap is resolved by
    // index, so the order is the resolution and getting it backwards would protect the
    // less important object.
    Sighting strong;
    strong.x1 = 100;
    strong.y1 = 100;
    strong.x2 = 300;
    strong.y2 = 200;
    strong.qp_delta = -12;
    Sighting weak = strong;
    weak.x1 = 200;
    weak.x2 = 400;
    weak.qp_delta = -4;
    const auto pair = builder.build(640, 480, {strong, weak}, q);
    CHECK(pair.size() >= 2);
    for (const Area& r : pair) {
        CHECK(usable(r, 640, 480));
    }
    CHECK(pair[0].qp_delta <= pair[1].qp_delta);

    // Deterministic: the same input twice is the same output twice, down to the rectangle
    // order.
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

/// When the budget cannot pay for everything, it pays for the important thing.
///
/// The shaped builder sorts ascending -- it paints onto a block map and the map keeps the
/// last write, so the important object has to be painted last -- and deciding affordability
/// in the same walk would serve the least important object in the frame first. The
/// rectangle builder beside it has no painting order to satisfy, so the two must agree
/// whether or not the backend has a block map.
void the_budget_pays_for_the_important_object_first()
{
    constexpr int kW = 1000;
    constexpr int kH = 1000;
    // Two equal squares of 9% of the frame each. The budget holds one of them, so which one
    // it holds is the whole assertion.
    constexpr double kSquare = 0.09;

    QualityPlan quality;
    quality.background_qp_delta = 6;
    quality.pad = 0;
    quality.max_protected_fraction = static_cast<float>(kSquare * 1.5);

    const auto square = [](int x, int y, int side, float importance, int32_t delta) {
        Sighting d;
        d.x1 = x;
        d.y1 = y;
        d.x2 = x + side;
        d.y2 = y + side;
        d.qp_delta = delta;
        d.pad_qp_delta = 0;
        d.importance = importance;
        // A polygon is what selects the shaped builder; a square one keeps the geometry the
        // same as the rectangle path so that only the ordering is under test. Outline
        // vertices are normalised to the frame, unlike the pixel bounds beside them.
        const float nx0 = static_cast<float>(x) / kW;
        const float ny0 = static_cast<float>(y) / kH;
        const float nx1 = static_cast<float>(x + side) / kW;
        const float ny1 = static_cast<float>(y + side) / kH;
        d.polygon = {{nx0, ny0}, {nx1, ny0}, {nx1, ny1}, {nx0, ny1}};
        return d;
    };

    const int side = static_cast<int>(std::sqrt(kSquare) * kW);
    // The important one second in the list, so that a builder serving them in arrival order
    // would fail this too.
    const std::vector<Sighting> dets{
        square(50, 50, side, 0.20f, -4),
        square(600, 600, side, 0.95f, -10),
    };

    RoiGridBuilder builder;
    const std::vector<Area> rects = builder.build(kW, kH, dets, quality, 16, 0);
    QpOffsetsBuilder qp_offsets;
    const auto grid = qp_offsets.build(kW, kH, 16, rects);

    const auto delta_at = [&](int x, int y) {
        return grid.deltas[static_cast<std::size_t>(y / 16) * grid.cols + (x / 16)];
    };
    // The important square keeps its ladder delta; the weak one is left neutral rather than
    // degraded -- a region nobody can afford to sharpen is still not background.
    CHECK(delta_at(600 + side / 2, 600 + side / 2) == -10);
    CHECK(delta_at(50 + side / 2, 50 + side / 2) == 0);
    // And the background around them is still background.
    CHECK(delta_at(kW - 8, 8) == 6);
}

/// The edge cases above are coordinates a caller can get wrong -- negative, past the edge,
/// zero area, tiny, overlapping -- and they are numbers. These are not.
///
/// The public API takes floats from whoever fills a Box, and nothing upstream promises they
/// are finite. A NaN would travel into std::lround (undefined for a NaN), into a
/// stable_sort comparator (where it breaks the strict weak ordering the standard requires)
/// and into the polygon rasteriser. It is dropped at the boundary, the way a degenerate box
/// is: the frame still encodes and one region is missing, rather than the process being
/// gone.
void a_box_carrying_a_number_arithmetic_cannot_use_is_dropped()
{
    RoiGridBuilder builder;
    QualityPlan quality;
    quality.background_qp_delta = 6;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const auto box_at = [](float x, float y, float w, float h, float confidence) {
        Box b;
        b.kind = "plate";
        b.x = x;
        b.y = y;
        b.width = w;
        b.height = h;
        b.confidence = confidence;
        return b;
    };

    std::vector<Box> boxes{
        box_at(nan, 0.1f, 0.2f, 0.2f, 0.9f),   // a coordinate
        box_at(0.1f, inf, 0.2f, 0.2f, 0.9f),   // an infinity
        box_at(0.1f, 0.1f, nan, 0.2f, 0.9f),   // an extent
        box_at(0.1f, 0.1f, 0.2f, 0.2f, nan),   // and the confidence, which becomes the
                                               // importance the sort is ordered by
        box_at(0.4f, 0.4f, 0.2f, 0.2f, 0.9f),  // the only usable one
    };
    // A polygon vertex is the fifth way in, and it reaches the rasteriser rather than
    // lround.
    Box shaped = box_at(0.6f, 0.6f, 0.2f, 0.2f, 0.9f);
    shaped.polygon = {{0.6f, 0.6f}, {nan, 0.8f}, {0.6f, 0.8f}};
    boxes.push_back(shaped);

    const auto dets = builder.detections_from(boxes, quality, 1000, 1000);
    CHECK(dets.size() == 1);
    // The survivor is the usable box: it covers the 0.4..0.6 square it was given. The
    // bounds are not asserted exactly because the margin widens a detection, and by how
    // much is that rule's business, not this test's.
    CHECK(dets[0].x1 <= 400 && dets[0].x2 >= 600);
    CHECK(dets[0].y1 <= 400 && dets[0].y2 >= 600);

    // And the frame still encodes: one usable region, plus the background.
    const auto rects = builder.build(1000, 1000, dets, quality);
    CHECK(!rects.empty());
    for (const Area& r : rects) {
        CHECK(r.left >= 0 && r.top >= 0 && r.right <= 1000 && r.bottom <= 1000);
        CHECK(r.left < r.right && r.top < r.bottom);
    }
}

} // namespace

int main()
{
    normalization_edge_cases();
    the_budget_pays_for_the_important_object_first();
    a_box_carrying_a_number_arithmetic_cannot_use_is_dropped();

    RoiGridBuilder builder;

    Sighting plate;
    plate.x1 = 800;
    plate.y1 = 500;
    plate.x2 = 960;
    plate.y2 = 560;
    plate.qp_delta = -10;
    plate.pad_qp_delta = -5;
    plate.pad_x = 40;
    plate.pad_y = 22;

    QualityPlan background_only;
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

    QualityPlan quality;
    quality.hold_sec = 0.25;
    // Keep the pad-as-halo semantics for this fixture; FixedPixels plus min side would
    // shift the AABB and is covered by region_rules_test.
    quality.box_margin = BoxMargin::FractionOfPicture;
    quality.min_roi_side = 0;
    quality.kinds["plate"] = ClassQuality{-10, 0.02};
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

    QpOffsetsBuilder qp_offsets;
    const auto grid = qp_offsets.build(1920, 1080, 32, rects);
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
    QualityPlan poly_q;
    poly_q.background_qp_delta = 6;
    poly_q.pad = 0;
    poly_q.box_margin = BoxMargin::FractionOfPicture;
    poly_q.min_roi_side = 0;
    poly_q.max_protected_fraction = 1; // one frame's worth, which this single box fits in
    poly_q.kinds["vehicle"] = ClassQuality{-8, 0.0};
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
    const auto poly_grid = qp_offsets.build(64, 64, 16, poly_rects);
    CHECK(poly_grid.cols == 4 && poly_grid.rows == 4);
    CHECK(poly_grid.deltas[3 * 4 + 0] == -8); // bottom-left block
    CHECK(poly_grid.deltas[0 * 4 + 3] == 6);  // top-right stays background

    // A naive resize drops the background (it is last). truncate_roi_rects keeps it and
    // spends the remaining slots on earlier objects.
    std::vector<ta_roienc::Area> many;
    for (int32_t i = 0; i < 10; ++i) {
        many.push_back({.top = 0, .bottom = 10, .left = i * 10, .right = i * 10 + 8,
            .qp_delta = -5});
    }
    many.push_back({.top = 0, .bottom = 100, .left = 0, .right = 100, .qp_delta = 14});
    int32_t lost = 0;
    ta_roienc::truncate_roi_rects(many, 4, 100, 100, &lost);
    CHECK(many.size() == 4);
    CHECK(lost == 7); // 10 objects -> 3 kept + background; 7 objects dropped
    CHECK(many.back().qp_delta == 14);
    CHECK(many.back().right == 100 && many.back().bottom == 100);

    std::puts("roi_map_builder_test ok");
    return 0;
}
