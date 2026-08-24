#pragma once

// What the detector found -- TZ v2.0 §10.
//
// The fields are the specification's, verbatim. Two of them do nothing yet: the encoder
// still gets its quality from Quality::kinds, and semantic_priority and task_priority are
// consumed by the importance model of §11, which does not exist. They are declared now
// because the type is the contract between the CV side and this library, and because a
// caller that already has these numbers should not have to invent a place to put them.
//
// Geometry is where §10 goes beyond what the encoder currently accepts. A polygon is
// reduced to its bounding box on the way in; the rasterisation that would preserve its
// shape belongs with the importance map, for the same reason a mask does.

#include "smartvideo/geometry.h"

#include <string>

#include <cstdint>
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

struct Point {
    float x = 0;
    float y = 0;
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
/// A polygon collapses to its extent and a mask to the extent of its non-zero pixels --
/// which is lossy, and is exactly what the shape-preserving path in §11 is for.
BoundingBox bounding_box_of(const Geometry& geometry);

} // namespace smartvideo
