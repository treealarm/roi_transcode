#pragma once

#include "importance.h"
#include "smartvideo/object.h"
#include "smartvideo/object_priority.h"
#include "smartvideo/types.h"

#include <vector>

namespace smartvideo {

// A box resolved to pixels, with its halo. Internal: it is a step in turning boxes into
// rectangles, and nothing outside this library has a reason to build one. It used to sit in
// the public types header, where the only argument for it was that a test could reach it --
// which is a reason to arrange the test differently, not to publish a type.
struct Detection {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    int32_t qp_delta = -6;
    int32_t pad_qp_delta = -3;
    int32_t pad_x = 0;
    int32_t pad_y = 0;

    /// Continuous importance (v2 arithmetic), kept so regions can be ordered finely.
    float importance = 0.0f;
    /// v3 ladder step derived from importance (or from a configured delta).
    ObjectPriority priority = ObjectPriority::Normal;
    uint64_t track_id = 0;

    /// Normalized outline, when the box carried one. Empty means the axis-aligned rectangle
    /// above is the whole of the geometry.
    std::vector<Point> polygon;
};

/// The §10 view of a box, which is what the importance model reads. Confidence of zero on
/// the way in means "not reported" and becomes the policy's assumed value here, so nothing
/// downstream has to know the difference. A polygon on the box becomes Polygon geometry.
Object object_from_box(const Box& box, const ImportancePolicy& policy);

class RoiMapBuilder {
public:
    /// Turns weighed detections into the rectangles an encoder takes: objects in importance
    /// order, then their halos, then the background. Reads `background_qp_delta` and
    /// `max_protected_fraction` from the quality settings -- the second is why this takes
    /// the whole struct rather than the one number it used to.
    ///
    /// When any detection carries a polygon the path goes through the block qp-map (same as
    /// a mask) so the outline is preserved; otherwise it stays on cheap axis-aligned rects.
    ///
    /// `coding_block` is the qp_map cell (16 for H.264, 32 for HEVC here).
    /// `map_region_limit`: <0 keeps the old Quality::max_regions default for the shaped
    /// path; 0 means unlimited (backend has qp_map); >0 truncates strongest-first.
    std::vector<Rect> build(int width, int height,
        const std::vector<Detection>& detections, const Quality& quality,
        int32_t coding_block = 16, int32_t map_region_limit = -1) const;

    // Boxes lie on a timeline; picks the ones within Quality::hold_sec of
    // `t_sec` and resolves them to pixels.
    std::vector<Detection> detections_at(const std::vector<Box>& boxes,
        const Quality& quality, int width, int height, double t_sec) const;

    // Same conversion without the time window: a pushed frame is handed the
    // boxes that are already its own, so there is nothing to select.
    std::vector<Detection> detections_from(const std::vector<Box>& boxes,
        const Quality& quality, int width, int height) const;
};

/// Cap `rects` to `budget`, always keeping a trailing full-frame background rect when one
/// is present. RoiMapBuilder puts the background last; a naive resize then drops it first,
/// and on VA-API the driver drops whatever is past eight by a second rule that ignores
/// importance -- so the background penalty §15 spends from never reaches the encoder.
void truncate_roi_rects(std::vector<Rect>& rects, int32_t budget, int32_t width, int32_t height,
    int32_t* dropped = nullptr);

} // namespace smartvideo
