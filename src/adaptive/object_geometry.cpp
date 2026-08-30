// Geometry: bounding boxes, and painting a polygon onto a mask.
//
// The encoder takes rectangles. A polygon becomes rectangles by the same route a grayscale
// mask does -- rasterise onto a pixel grid, sample onto the block qp-map, merge runs -- so a
// corner outside the outline is not protected. The AABB of the same outline stays available
// for anything that only draws boxes.

#include "sve/object.h"
#include "sve/policy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sve {

namespace {

BoundingBox extent_of(const Polygon& poly)
{
    if (poly.points.empty()) {
        return {};
    }
    float min_x = poly.points[0].x, max_x = min_x;
    float min_y = poly.points[0].y, max_y = min_y;
    for (const Point& p : poly.points) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
    return {min_x, min_y, max_x - min_x, max_y - min_y};
}

BoundingBox extent_of(const Mask& mask)
{
    // consistent() is the whole check: it says the span really holds the rows the
    // dimensions claim, which is what the old pointer form could not express.
    if (!mask.consistent()) {
        return {};
    }
    const int stride = mask.stride > 0 ? mask.stride : mask.width;
    int min_x = mask.width, max_x = -1, min_y = mask.height, max_y = -1;
    for (int y = 0; y < mask.height; ++y) {
        const uint8_t* row = mask.data.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < mask.width; ++x) {
            if (row[x] == 0) {
                continue;
            }
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < 0) {
        return {};  // nothing set: an empty mask asks for nothing
    }
    // +1 on the extents because a pixel covers the span up to the next one.
    const float w = static_cast<float>(mask.width);
    const float h = static_cast<float>(mask.height);
    return {
        static_cast<float>(min_x) / w,
        static_cast<float>(min_y) / h,
        static_cast<float>(max_x - min_x + 1) / w,
        static_cast<float>(max_y - min_y + 1) / h,
    };
}

} // namespace

BoundingBox bounding_box_of(const Geometry& geometry)
{
    return std::visit([](const auto& g) -> BoundingBox {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, BoundingBox>) {
            return g;
        } else {
            return extent_of(g);
        }
    }, geometry);
}

bool rasterize_polygon(const Polygon& polygon, uint8_t* out, int32_t width, int32_t height,
    uint8_t value)
{
    if (!out || width <= 0 || height <= 0 || polygon.points.size() < 3) {
        return false;
    }

    // Scanline even-odd fill. Points are normalized; a vertex on the right/bottom edge is
    // clamped into the last pixel so a polygon that touches 1.0 still paints something.
    const int n = static_cast<int>(polygon.points.size());
    std::vector<float> xs(static_cast<size_t>(n));
    std::vector<float> ys(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        xs[static_cast<size_t>(i)] =
            std::clamp(polygon.points[static_cast<size_t>(i)].x, 0.0f, 1.0f)
            * static_cast<float>(width);
        ys[static_cast<size_t>(i)] =
            std::clamp(polygon.points[static_cast<size_t>(i)].y, 0.0f, 1.0f)
            * static_cast<float>(height);
    }

    float min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < n; ++i) {
        min_y = std::min(min_y, ys[static_cast<size_t>(i)]);
        max_y = std::max(max_y, ys[static_cast<size_t>(i)]);
    }
    const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(max_y)));

    std::vector<float> crossings;
    crossings.reserve(static_cast<size_t>(n));

    for (int y = y0; y <= y1; ++y) {
        const float y_scan = static_cast<float>(y) + 0.5f;
        crossings.clear();
        for (int i = 0; i < n; ++i) {
            const float y_a = ys[static_cast<size_t>(i)];
            const float y_b = ys[static_cast<size_t>((i + 1) % n)];
            const float x_a = xs[static_cast<size_t>(i)];
            const float x_b = xs[static_cast<size_t>((i + 1) % n)];
            // Skip horizontal edges; require the scanline to cross strictly upward or downward.
            if ((y_a <= y_scan && y_b > y_scan) || (y_b <= y_scan && y_a > y_scan)) {
                const float t = (y_scan - y_a) / (y_b - y_a);
                crossings.push_back(x_a + t * (x_b - x_a));
            }
        }
        std::sort(crossings.begin(), crossings.end());
        uint8_t* row = out + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            int x_left = static_cast<int>(std::floor(crossings[i]));
            int x_right = static_cast<int>(std::ceil(crossings[i + 1]));
            x_left = std::max(0, x_left);
            x_right = std::min(width, x_right);
            for (int x = x_left; x < x_right; ++x) {
                row[x] = value;
            }
        }
    }
    return true;
}

void dilate_mask(uint8_t* mask, int32_t width, int32_t height, int32_t radius)
{
    if (!mask || width <= 0 || height <= 0 || radius <= 0) {
        return;
    }
    // Two-pass separable max-filter approximation with a square kernel: dilate horizontally,
    // then vertically. Exact for a binary mask and cheap enough for a frame-sized buffer.
    std::vector<uint8_t> tmp(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = mask + static_cast<size_t>(y) * width;
        uint8_t* dst = tmp.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            uint8_t peak = 0;
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(width - 1, x + radius);
            for (int xx = x0; xx <= x1; ++xx) {
                peak = std::max(peak, src[xx]);
            }
            dst[x] = peak;
        }
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* dst = mask + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            uint8_t peak = 0;
            const int y0 = std::max(0, y - radius);
            const int y1 = std::min(height - 1, y + radius);
            for (int yy = y0; yy <= y1; ++yy) {
                peak = std::max(peak,
                    tmp[static_cast<size_t>(yy) * width + static_cast<size_t>(x)]);
            }
            dst[x] = peak;
        }
    }
}

const char* quality_policy_name(QualityPolicy p)
{
    switch (p) {
    case QualityPolicy::OBJECT_FIRST:  return "OBJECT_FIRST";
    case QualityPolicy::BALANCED:      return "BALANCED";
    case QualityPolicy::BITRATE_FIRST: return "BITRATE_FIRST";
    }
    return "?";
}

} // namespace sve
