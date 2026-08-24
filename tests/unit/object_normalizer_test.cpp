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

    std::puts("roi_map_builder_test ok");
    return 0;
}
