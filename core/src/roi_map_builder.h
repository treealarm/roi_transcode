#pragma once

#include "roi/roi_types.h"

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

    // Same conversion without the time window: the pushed frame mode is handed
    // the boxes for the frame it is submitting, so there is nothing to select.
    std::vector<Detection> detections_from(
        const std::vector<RoiSample>& samples,
        const std::unordered_map<std::string, KindConfig>& kinds,
        int width,
        int height) const;
};
