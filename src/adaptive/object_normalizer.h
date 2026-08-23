#pragma once

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
};

class RoiMapBuilder {
public:
    std::vector<Rect> build(int width, int height,
        const std::vector<Detection>& detections, int background_qp_delta) const;

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
