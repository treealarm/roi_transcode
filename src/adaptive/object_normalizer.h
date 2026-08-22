#pragma once

#include "smartvideo/types.h"

#include <vector>

namespace roi {

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

} // namespace roi
