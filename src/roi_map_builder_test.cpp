#include "qp_delta_map.h"
#include "roi_map_builder.h"

#include <cstdio>
#include <cstdlib>

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

    const auto rects = builder.build(1920, 1080, {plate}, 6);
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

    RoiDocument doc;
    doc.hold_sec = 0.25;
    doc.kinds["plate"] = KindConfig{-10, 0.02};
    doc.samples.push_back(RoiSample{
        .t = 1.25,
        .kind = "plate",
        .bbox_x = 0.5f,
        .bbox_y = 0.5f,
        .bbox_w = 0.1f,
        .bbox_h = 0.05f,
    });

    const auto at = builder.detections_at(doc, 1000, 1000, 1.20);
    CHECK(at.size() == 1);
    CHECK(at[0].x1 == 500);
    CHECK(at[0].x2 == 600);
    CHECK(at[0].qp_delta == -10);

    const auto miss = builder.detections_at(doc, 1000, 1000, 9.0);
    CHECK(miss.empty());

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
