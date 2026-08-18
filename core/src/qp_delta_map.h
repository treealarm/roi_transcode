#pragma once

#include "roi/geometry.h"
#include "roi/roi_types.h"

#include <cstdint>
#include <vector>

// NVIDIA NVENC qpDeltaMap: signed QP delta per CTU/MB, raster order.
// H.264 = 16×16, HEVC = 32×32. First RoiRect wins on overlap (same as VPL).

class QpDeltaMapBuilder {
public:
    QpDeltaMap build(
        int width,
        int height,
        int block,
        const std::vector<RoiRect>& rects) const;
};
