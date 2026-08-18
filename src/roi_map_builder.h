#pragma once

#include "roi_types.h"

#include <vector>

class RoiMapBuilder {
public:
    std::vector<RoiRect> build(
        int width,
        int height,
        const std::vector<Detection>& detections,
        int background_qp_delta) const;

    std::vector<Detection> detections_at(
        const RoiDocument& doc,
        int width,
        int height,
        double t_sec) const;
};
