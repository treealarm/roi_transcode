#pragma once

// What the detector found -- TZ v2.0 §10.
//
// Geometry is where §10 goes beyond an axis-aligned box. A polygon used to collapse to its
// AABB on the way in; the shape-preserving path paints it onto the same block qp-map a mask
// uses, so a corner outside the outline is not protected. BoundingBox stays the cheap form.

#include "smartvideo/geometry.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace smartvideo {

/// Normalized 0..1 from the top-left corner -- the convention every geometry here shares,
/// so a detection survives a change of resolution.
struct BoundingBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

struct Polygon {
    std::vector<Point> points;
};

/// BoundingBox, Polygon or BinaryMask (§2). Mask holds a borrowed pointer, so an Object
/// carrying one must not outlive the pixels it points at.
using Geometry = std::variant<BoundingBox, Polygon, Mask>;

struct Object {
    uint64_t track_id = 0;
    uint32_t class_id = 0;
    /// The class as a name -- "person", "face", "vehicle", "license_plate".
    ///
    /// §10 has only the numeric class_id. A number would mean a second table mapping it to
    /// the names everything else here already speaks: ONVIF reports classes as text, the
    /// archive stores text, Quality::kinds is keyed by text. That table would be one more
    /// place for the two to disagree, so the name travels and class_id stays for a caller
    /// who has one.
    std::string kind;
    float confidence = 1.0f;
    Geometry geometry;

    /// How much this class is worth in general -- the "license plate 1.00, vehicle 0.60"
    /// column of §11. Consumed by the importance model, not by the encoder directly.
    float semantic_priority = 0.5f;
    /// How much it is worth to the task at hand, which may differ from the class alone:
    /// a plate matters more to ALPR than to counting pedestrians.
    float task_priority = 1.0f;
    /// Never sacrificed when the bitrate budget runs short (§15).
    bool critical = false;
};

/// The smallest axis-aligned box containing this geometry, normalized like the input.
/// A polygon collapses to its extent and a mask to the extent of its non-zero pixels.
BoundingBox bounding_box_of(const Geometry& geometry);

/// Fills `out` (tightly packed `width * height`) with `value` inside the polygon, using the
/// even-odd rule. Points are normalized 0..1. Fewer than three points leaves `out` untouched
/// and returns false. Does not clear pixels outside the outline -- the caller zeros the
/// buffer when a fresh mask is wanted.
bool rasterize_polygon(const Polygon& polygon, uint8_t* out, int32_t width, int32_t height,
    uint8_t value = 255);

/// Square dilation of a binary mask in place. `radius` is in pixels; 0 is a no-op. Used to
/// grow a polygon by Quality::pad before it is sampled onto the block grid.
void dilate_mask(uint8_t* mask, int32_t width, int32_t height, int32_t radius);

} // namespace smartvideo
