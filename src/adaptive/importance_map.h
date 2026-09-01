#pragma once

#include "sve/geometry.h"
#include "sve/types.h"

#include <cstdint>
#include <span>
#include <vector>
namespace sve {


// NVIDIA NVENC qpDeltaMap: signed QP delta per CTU/MB, raster order.
// H.264 = 16×16, HEVC = 32×32. First Rect wins on overlap (same as VPL).

/// Cells across and down for a frame of `width`/`height` painted at `block`.
/// One definition, because a grid sized differently from the one it is painted into is
/// the shape of an out-of-bounds write rather than a rounding difference.
constexpr int qp_map_cols(int width, int block)
{
    return (width <= 0 || block <= 0) ? 0 : (width + block - 1) / block;
}
constexpr int qp_map_rows(int height, int block)
{
    return qp_map_cols(height, block);
}

class QpDeltaMapBuilder {
public:
    QpDeltaMap build(int width, int height, int block, std::span<const Rect> rects) const;

    /// The same painting rule, into a buffer the caller already owns.
    ///
    /// Separate from build() for the one caller that runs per frame: a backend handing the
    /// grid to hardware needs the cells, not a QpDeltaMap, and building one per frame would
    /// allocate sixty times a second for a vector nobody keeps. `out` must hold
    /// qp_map_cols x qp_map_rows cells and is *not* cleared -- the caller owns the buffer
    /// and knows whether it is reused.
    void paint(int width, int height, int block, std::span<const Rect> rects,
        std::span<int8_t> out) const;
};

} // namespace sve
