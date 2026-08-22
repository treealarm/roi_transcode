#pragma once

// ROI geometry: turning what a caller knows about a frame -- a mask, or a list
// of boxes -- into the rectangles an encoder accepts. Pure arithmetic, no
// FFmpeg, which is why it is exposed: these are the parts worth unit testing.

#include "smartvideo/types.h"

#include <cstdint>
#include <vector>

namespace roi {

// Signed QP delta per coding block, raster order. HEVC uses 32x32 CTUs,
// H.264 16x16 macroblocks. This is the common representation a mask and a
// box list both reduce to.
struct QpDeltaMap {
    int block = 32;
    int cols = 0;
    int rows = 0;
    // Frame extent the grid was built for. The last row and column of blocks are
    // partial whenever it is not a multiple of `block`, and a rectangle derived
    // from the grid has to stop at the real edge rather than past it.
    int width = 0;
    int height = 0;
    std::vector<int8_t> deltas;
};

// 8-bit region-of-interest mask at any resolution: 0 is background, 255 is
// maximum quality. It is sampled onto the frame, so it need not match its size.
struct Mask {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0; // bytes per row; 0 means tightly packed
};

// How mask levels become QP deltas. Fewer levels means fewer distinct values,
// and therefore fewer rectangles to spend the encoder's region budget on.
struct MaskMapping {
    int qp_delta_at_255 = -6; // strongest ROI
    int qp_delta_at_0 = 6;    // background
    int levels = 4;
};

// Samples `mask` onto the frame's block grid. Each block takes the *strongest*
// quality it covers, not the average: crushing a block that caught the edge of
// a plate costs more than spending bits on the background beside it.
QpDeltaMap qp_map_from_mask(const Mask& mask, const MaskMapping& mapping,
    int frame_width, int frame_height, int block = 32);

// Merges equal-delta blocks into rectangles, strongest |delta| first because the
// lowest index wins on overlap. Blocks at delta 0 are left out: zero means "no
// change", which is the absence of a region rather than a region asking for
// nothing. Truncated to `max_regions` -- mfxExtEncoderROI takes at most 256 and
// a VAAPI driver usually far fewer -- and `dropped`, if given, reports the cut.
std::vector<Rect> rects_from_qp_map(
    const QpDeltaMap& map, int max_regions, int* dropped = nullptr);

} // namespace roi
