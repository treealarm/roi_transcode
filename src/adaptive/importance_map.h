#pragma once

#include "sve/geometry.h"
#include "sve/types.h"

#include <cstdint>
#include <vector>
namespace sve {


// NVIDIA NVENC qpDeltaMap: signed QP delta per CTU/MB, raster order.
// H.264 = 16×16, HEVC = 32×32. First Rect wins on overlap (same as VPL).

class QpDeltaMapBuilder {
public:
    QpDeltaMap build(
        int width,
        int height,
        int block,
        const std::vector<Rect>& rects) const;
};

} // namespace sve
