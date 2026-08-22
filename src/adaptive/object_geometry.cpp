// Geometry down to a bounding box.
//
// Lossy on purpose and only here: the encoder takes rectangles, so a polygon or a mask has
// to become one somewhere. Doing it at the edge keeps the loss visible instead of hiding
// it inside the importance map, which is where the shape-preserving path will live (§11).

#include "smartvideo/object.h"
#include "smartvideo/policy.h"

#include <algorithm>

namespace smartvideo {

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
    if (!mask.data || mask.width <= 0 || mask.height <= 0) {
        return {};
    }
    const int stride = mask.stride > 0 ? mask.stride : mask.width;
    int min_x = mask.width, max_x = -1, min_y = mask.height, max_y = -1;
    for (int y = 0; y < mask.height; ++y) {
        const uint8_t* row = mask.data + static_cast<size_t>(y) * stride;
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

const char* quality_policy_name(QualityPolicy p)
{
    switch (p) {
    case QualityPolicy::OBJECT_FIRST:  return "OBJECT_FIRST";
    case QualityPolicy::BALANCED:      return "BALANCED";
    case QualityPolicy::BITRATE_FIRST: return "BITRATE_FIRST";
    }
    return "?";
}

} // namespace smartvideo
