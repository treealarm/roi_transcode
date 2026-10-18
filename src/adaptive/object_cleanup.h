#pragma once

#include "weights.h"
#include "ta_roienc/tracked_object.h"
#include "ta_roienc/salience.h"
#include "ta_roienc/basics.h"

#include <vector>

namespace ta_roienc {

// A box resolved to pixels, with its halo. Internal: it is a step in turning boxes into
// rectangles, and nothing outside this library has a reason to build one.
struct Sighting {
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
    /// Priority-ladder step derived from importance (or from a configured delta).
    Salience priority = Salience::Normal;
    uint64_t track_id = 0;

    /// Normalized outline, when the box carried one. Empty means the axis-aligned rectangle
    /// above is the whole of the geometry.
    std::vector<Vertex> polygon;
};

/// The object view of a box, which is what the importance model reads. Confidence of zero on
/// the way in means "not reported" and becomes the policy's assumed value here, so nothing
/// downstream has to know the difference. A polygon on the box becomes Outline geometry.
Object object_from_box(const Box& box, const SalienceRules& policy);

class RoiGridBuilder {
public:
    /// Turns weighed detections into the rectangles an encoder takes: objects in importance
    /// order, then their halos, then the background. Reads `background_qp_delta` and
    /// `max_protected_fraction` from the quality settings.
    ///
    /// When any detection carries a polygon the path goes through the block qp-map (same as
    /// a mask) so the outline is preserved; otherwise it stays on cheap axis-aligned rects.
    ///
    /// `coding_block` is the qp_offsets cell (16 for H.264, 32 for HEVC here).
    /// `map_region_limit`: <0 keeps the QualityPlan::max_regions default for the shaped path;
    /// 0 means unlimited (backend has qp_offsets); >0 truncates strongest-first.
    ///
    /// `dropped`, when given, receives how many regions the shaped path had to discard to
    /// fit that limit, so that `stats.regions_dropped` and `guard_roi_regions_dropped` see
    /// it on exactly the crowded scenes the limit exists for. The rectangle path has no such
    /// step -- its truncation happens in the caller, through truncate_roi_rects, which
    /// counts.
    std::vector<Area> build(int width, int height,
        const std::vector<Sighting>& detections, const QualityPlan& quality,
        int32_t coding_block = 16, int32_t map_region_limit = -1,
        int32_t* dropped = nullptr) const;

    // Boxes lie on a timeline; picks the ones within QualityPlan::hold_sec of
    // `t_sec` and resolves them to pixels.
    std::vector<Sighting> detections_at(const std::vector<Box>& boxes,
        const QualityPlan& quality, int width, int height, double t_sec) const;

    // Same conversion without the time window: a pushed frame is handed the
    // boxes that are already its own, so there is nothing to select.
    std::vector<Sighting> detections_from(const std::vector<Box>& boxes,
        const QualityPlan& quality, int width, int height) const;
};

/// Cap `rects` to `budget`, always keeping a trailing full-frame background rect when one
/// is present. RoiGridBuilder puts the background last; a naive resize then drops it first,
/// and on VA-API the driver drops whatever is past eight by a second rule that ignores
/// importance -- so the background penalty the budget spends from never reaches the
/// encoder.
void truncate_roi_rects(std::vector<Area>& rects, int32_t budget, int32_t width, int32_t height,
    int32_t* dropped = nullptr);

} // namespace ta_roienc
