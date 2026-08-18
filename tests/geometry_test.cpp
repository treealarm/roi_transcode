#include "roi/geometry.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

RoiMask mask_of(std::vector<uint8_t>& buf, int w, int h)
{
    buf.assign(static_cast<size_t>(w) * h, 0);
    RoiMask m;
    m.data = buf.data();
    m.width = w;
    m.height = h;
    m.stride = w;
    return m;
}

const RoiRect* find_rect(const std::vector<RoiRect>& v, int left, int top)
{
    for (const auto& r : v) {
        if (r.left == left && r.top == top) {
            return &r;
        }
    }
    return nullptr;
}

void test_mapping_endpoints()
{
    std::vector<uint8_t> buf;
    RoiMask m = mask_of(buf, 64, 64);
    MaskMapping mp; // -6 at 255, +6 at 0, 4 levels

    // All background.
    QpDeltaMap grid = qp_map_from_mask(m, mp, 64, 64, 32);
    CHECK(grid.cols == 2 && grid.rows == 2);
    CHECK(grid.width == 64 && grid.height == 64);
    for (auto d : grid.deltas) {
        CHECK(d == 6);
    }

    // Full strength everywhere.
    buf.assign(buf.size(), 255);
    grid = qp_map_from_mask(m, mp, 64, 64, 32);
    for (auto d : grid.deltas) {
        CHECK(d == -6);
    }
}

void test_block_takes_the_strongest()
{
    std::vector<uint8_t> buf;
    RoiMask m = mask_of(buf, 64, 64);
    MaskMapping mp;

    // A single lit pixel in the top-left block must carry that whole block:
    // the block takes the peak, not the mean.
    buf[0] = 255;
    const QpDeltaMap grid = qp_map_from_mask(m, mp, 64, 64, 32);
    CHECK(grid.deltas[0] == -6);
    CHECK(grid.deltas[1] == 6);
    CHECK(grid.deltas[2] == 6);
    CHECK(grid.deltas[3] == 6);
}

void test_mask_is_sampled_not_resized()
{
    // Mask a quarter of the frame's size, lit on its left half. The lit half
    // must land on the frame's left half regardless of the scale difference.
    std::vector<uint8_t> buf;
    RoiMask m = mask_of(buf, 16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 8; ++x) {
            buf[static_cast<size_t>(y) * 16 + x] = 255;
        }
    }
    MaskMapping mp;
    const QpDeltaMap grid = qp_map_from_mask(m, mp, 128, 128, 32);
    CHECK(grid.cols == 4 && grid.rows == 4);
    for (int y = 0; y < 4; ++y) {
        CHECK(grid.deltas[static_cast<size_t>(y) * 4 + 0] == -6);
        CHECK(grid.deltas[static_cast<size_t>(y) * 4 + 1] == -6);
        CHECK(grid.deltas[static_cast<size_t>(y) * 4 + 2] == 6);
        CHECK(grid.deltas[static_cast<size_t>(y) * 4 + 3] == 6);
    }
}

void test_rects_merge_and_clamp()
{
    // 96x80 frame, 32px blocks: 3 columns, 3 rows, the last row only 16px tall.
    QpDeltaMap grid;
    grid.block = 32;
    grid.cols = 3;
    grid.rows = 3;
    grid.width = 96;
    grid.height = 80;
    grid.deltas.assign(9, 0);
    // Left two columns of the top two rows at -8: one 64x64 rectangle.
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            grid.deltas[static_cast<size_t>(y) * 3 + x] = -8;
        }
    }
    int dropped = -1;
    const auto rects = rects_from_qp_map(grid, 32, &dropped);
    CHECK(dropped == 0);
    CHECK(rects.size() == 1);
    CHECK(rects[0].left == 0);
    CHECK(rects[0].right == 64);
    CHECK(rects[0].top == 0);
    CHECK(rects[0].bottom == 64);
    CHECK(rects[0].qp_delta == -8);

    // A full-height column on the right clamps to the frame, not to 3*32.
    grid.deltas.assign(9, 0);
    for (int y = 0; y < 3; ++y) {
        grid.deltas[static_cast<size_t>(y) * 3 + 2] = 5;
    }
    const auto edge = rects_from_qp_map(grid, 32, nullptr);
    CHECK(edge.size() == 1);
    CHECK(edge[0].right == 96);
    CHECK(edge[0].bottom == 80); // not 96
}

void test_zero_is_absence_not_a_region()
{
    QpDeltaMap grid;
    grid.block = 16;
    grid.cols = 2;
    grid.rows = 1;
    grid.width = 32;
    grid.height = 16;
    grid.deltas = {0, -4};
    const auto rects = rects_from_qp_map(grid, 32, nullptr);
    CHECK(rects.size() == 1);
    CHECK(rects[0].left == 16);
    CHECK(rects[0].qp_delta == -4);
}

void test_priority_order_and_truncation()
{
    // Four separated single-block regions of increasing strength.
    QpDeltaMap grid;
    grid.block = 16;
    grid.cols = 7;
    grid.rows = 1;
    grid.width = 112;
    grid.height = 16;
    grid.deltas = {-2, 0, -9, 0, 4, 0, -6};

    const auto all = rects_from_qp_map(grid, 32, nullptr);
    CHECK(all.size() == 4);
    // Strongest |delta| first: -9, -6, 4, -2.
    CHECK(all[0].qp_delta == -9);
    CHECK(all[1].qp_delta == -6);
    CHECK(all[2].qp_delta == 4);
    CHECK(all[3].qp_delta == -2);

    int dropped = 0;
    const auto few = rects_from_qp_map(grid, 2, &dropped);
    CHECK(few.size() == 2);
    CHECK(dropped == 2);
    CHECK(few[0].qp_delta == -9);
    CHECK(few[1].qp_delta == -6);
    CHECK(find_rect(few, 0, 0) == nullptr); // the weakest was the one dropped
}

void test_degenerate_input()
{
    MaskMapping mp;
    RoiMask empty;
    CHECK(qp_map_from_mask(empty, mp, 64, 64, 32).deltas.empty());

    std::vector<uint8_t> buf;
    RoiMask m = mask_of(buf, 8, 8);
    CHECK(qp_map_from_mask(m, mp, 0, 64, 32).deltas.empty());
    CHECK(qp_map_from_mask(m, mp, 64, 64, 0).deltas.empty());
    CHECK(rects_from_qp_map(QpDeltaMap{}, 32, nullptr).empty());
}

} // namespace

int main()
{
    test_mapping_endpoints();
    test_block_takes_the_strongest();
    test_mask_is_sampled_not_resized();
    test_rects_merge_and_clamp();
    test_zero_is_absence_not_a_region();
    test_priority_order_and_truncation();
    test_degenerate_input();
    std::puts("geometry_test ok");
    return 0;
}
