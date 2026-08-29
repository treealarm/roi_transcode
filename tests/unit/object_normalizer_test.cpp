#include "importance_map.h"
#include "object_normalizer.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace smartvideo;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

int main()
{
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
    std::vector<smartvideo::Rect> many;
    for (int32_t i = 0; i < 10; ++i) {
        many.push_back({.top = 0, .bottom = 10, .left = i * 10, .right = i * 10 + 8,
            .qp_delta = -5});
    }
    many.push_back({.top = 0, .bottom = 100, .left = 0, .right = 100, .qp_delta = 14});
    int32_t lost = 0;
    smartvideo::truncate_roi_rects(many, 4, 100, 100, &lost);
    CHECK(many.size() == 4);
    CHECK(lost == 7); // 10 objects -> 3 kept + background; 7 objects dropped
    CHECK(many.back().qp_delta == 14);
    CHECK(many.back().right == 100 && many.back().bottom == 100);

    std::puts("roi_map_builder_test ok");
    return 0;
}
