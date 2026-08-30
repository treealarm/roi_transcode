// bounding_box_of: polygon and mask reduced to the rectangle the encoder can take.

#include "sve/object.h"

#include <cmath>
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

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void box_passes_through()
{
    const BoundingBox in{0.25f, 0.5f, 0.1f, 0.2f};
    const BoundingBox out = bounding_box_of(in);
    CHECK(near(out.x, 0.25f) && near(out.y, 0.5f));
    CHECK(near(out.width, 0.1f) && near(out.height, 0.2f));
}

void polygon_collapses_to_its_extent()
{
    Polygon p;
    p.points = {{0.2f, 0.3f}, {0.6f, 0.1f}, {0.4f, 0.7f}};
    const BoundingBox out = bounding_box_of(p);
    CHECK(near(out.x, 0.2f) && near(out.y, 0.1f));
    CHECK(near(out.width, 0.4f) && near(out.height, 0.6f));
}

void empty_polygon_asks_for_nothing()
{
    const BoundingBox out = bounding_box_of(Polygon{});
    CHECK(near(out.width, 0.0f) && near(out.height, 0.0f));
}

void mask_extent_covers_the_set_pixels()
{
    // 10x10, non-zero from (2,3) to (5,7) inclusive. The extent must include the last
    // pixel's full span, not stop at its origin -- a rectangle that ends one pixel early
    // leaves the edge of an object outside the region meant to protect it.
    std::vector<uint8_t> buf(100, 0);
    for (int y = 3; y <= 7; ++y) {
        for (int x = 2; x <= 5; ++x) {
            buf[static_cast<size_t>(y) * 10 + x] = 200;
        }
    }
    Mask m;
    m.data = buf;
    m.width = 10;
    m.height = 10;
    m.stride = 10;

    const BoundingBox out = bounding_box_of(m);
    CHECK(near(out.x, 0.2f) && near(out.y, 0.3f));
    CHECK(near(out.width, 0.4f));   // x 2..5 inclusive = 4 of 10
    CHECK(near(out.height, 0.5f));  // y 3..7 inclusive = 5 of 10
}

void empty_mask_asks_for_nothing()
{
    std::vector<uint8_t> buf(64, 0);
    Mask m;
    m.data = buf;
    m.width = 8;
    m.height = 8;
    m.stride = 8;
    const BoundingBox out = bounding_box_of(m);
    CHECK(near(out.width, 0.0f) && near(out.height, 0.0f));
}

void rasterize_fills_the_triangle_and_leaves_the_corner()
{
    // Right triangle covering the bottom-left half of a 32x32 frame. The top-right
    // pixel must stay clear -- that is the whole point of not collapsing to the AABB.
    Polygon p;
    p.points = {{0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    std::vector<uint8_t> buf(32 * 32, 0);
    CHECK(rasterize_polygon(p, buf.data(), 32, 32, 255));
    CHECK(buf[31] == 0);            // top-right
    CHECK(buf[31 * 32] == 255);     // bottom-left
    CHECK(buf[31 * 32 + 15] != 0);  // mid of the base, inside
}

void degenerate_polygon_is_refused()
{
    std::vector<uint8_t> buf(16, 7);
    Polygon p;
    p.points = {{0.1f, 0.1f}, {0.9f, 0.9f}}; // two points
    CHECK(!rasterize_polygon(p, buf.data(), 4, 4, 255));
    CHECK(buf[0] == 7); // untouched
}

void dilate_grows_a_single_pixel()
{
    std::vector<uint8_t> buf(9, 0);
    buf[4] = 255; // centre of 3x3
    dilate_mask(buf.data(), 3, 3, 1);
    for (uint8_t v : buf) {
        CHECK(v == 255);
    }
}

} // namespace

int main()
{
    box_passes_through();
    polygon_collapses_to_its_extent();
    empty_polygon_asks_for_nothing();
    mask_extent_covers_the_set_pixels();
    empty_mask_asks_for_nothing();
    rasterize_fills_the_triangle_and_leaves_the_corner();
    degenerate_polygon_is_refused();
    dilate_grows_a_single_pixel();
    std::printf("object_geometry ok\n");
    return 0;
}
