#pragma once

// What the detector found.
//
// Geometry goes beyond an axis-aligned box: a polygon is painted onto the same block qp-map
// a mask uses, so a corner outside the outline is not protected. Extent stays the
// cheap form.

#include "ta_roienc/shapes.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ta_roienc {

/// Normalized 0..1 from the top-left corner -- the convention every geometry here shares,
/// so a detection survives a change of resolution.
struct Extent {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

struct Outline {
    std::vector<Vertex> points;
};

/// Extent, Outline or BinaryMask. Mask holds a borrowed pointer, so an Object
/// carrying one must not outlive the pixels it points at.
using Geometry = std::variant<Extent, Outline, Mask>;

struct Object {
    uint64_t track_id = 0;
    uint32_t class_id = 0;
    /// The class as a name -- "person", "face", "vehicle", "license_plate".
    ///
    /// A name rather than only a numeric class_id: a number would need a second table
    /// mapping it to the names everything else here already speaks -- ONVIF reports classes
    /// as text, the archive stores text, QualityPlan::kinds is keyed by text. That table would
    /// be one more place for the two to disagree, so the name travels and class_id stays
    /// for a caller who has one.
    std::string kind;
    float confidence = 1.0f;
    Geometry geometry;

    /// How much this class is worth in general -- "license plate 1.00, vehicle 0.60".
    /// Consumed by the importance model, not by the encoder directly.
    float semantic_salience = 0.5f;
    /// How much it is worth to the task at hand, which may differ from the class alone:
    /// a plate matters more to ALPR than to counting pedestrians.
    float task_salience = 1.0f;
    /// Never sacrificed when the bitrate budget runs short.
    bool critical = false;
};

/// The smallest axis-aligned box containing this geometry, normalized like the input.
/// A polygon collapses to its extent and a mask to the extent of its non-zero pixels.
Extent extent_from(const Geometry& geometry);

/// Fills `out` (tightly packed `width * height`) with `value` inside the polygon, using the
/// even-odd rule. Points are normalized 0..1. Fewer than three points leaves `out` untouched
/// and returns false. Does not clear pixels outside the outline -- the caller zeros the
/// buffer when a fresh mask is wanted.
bool fill_outline(const Outline& polygon, uint8_t* out, int32_t width, int32_t height,
    uint8_t value = static_cast<uint8_t>(kMaskMax));

/// Square dilation of a binary mask in place. `radius` is in pixels; 0 is a no-op. Used to
/// grow a polygon by QualityPlan::pad before it is sampled onto the block grid.
void grow_mask(uint8_t* mask, int32_t width, int32_t height, int32_t radius);

} // namespace ta_roienc
