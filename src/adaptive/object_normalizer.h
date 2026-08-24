#pragma once

#include "importance.h"
#include "smartvideo/object.h"
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

    /// What §11 said this is worth, kept so regions can be ordered by it. Only meaningful
    /// for detections the model weighed; one whose kind was configured explicitly carries
    /// the importance its qp_delta implies, so the ordering stays consistent either way.
    float importance = 0.0f;
};

/// The §10 view of a box, which is what the importance model reads. Confidence of zero on
/// the way in means "not reported" and becomes the policy's assumed value here, so nothing
/// downstream has to know the difference.
Object object_from_box(const Box& box, const ImportancePolicy& policy);

class RoiMapBuilder {
public:
    /// Turns weighed detections into the rectangles an encoder takes: objects in importance
    /// order, then their halos, then the background. Reads `background_qp_delta` and
    /// `max_protected_fraction` from the quality settings -- the second is why this takes
    /// the whole struct rather than the one number it used to.
    std::vector<Rect> build(int width, int height,
        const std::vector<Detection>& detections, const Quality& quality) const;

    // Boxes lie on a timeline; picks the ones within Quality::hold_sec of
    // `t_sec` and resolves them to pixels.
    std::vector<Detection> detections_at(const std::vector<Box>& boxes,
        const Quality& quality, int width, int height, double t_sec) const;

    // Same conversion without the time window: a pushed frame is handed the
    // boxes that are already its own, so there is nothing to select.
    std::vector<Detection> detections_from(const std::vector<Box>& boxes,
        const Quality& quality, int width, int height) const;
};

} // namespace smartvideo
