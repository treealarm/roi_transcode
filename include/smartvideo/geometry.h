#pragma once

// ROI geometry: turning what a caller knows about a frame -- a mask, or a list
// of boxes -- into the rectangles an encoder accepts. Pure arithmetic, no
// FFmpeg, which is why it is exposed: these are the parts worth unit testing.

#include "smartvideo/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace smartvideo {

// Signed QP delta per coding block, raster order. Cell size comes from the backend's
// EncoderCapabilities::qp_map_block_* (H.264 16x16 macroblocks; HEVC painted at 32x32
// here). This is the common representation a mask and a box list both reduce to.
struct QpDeltaMap {
    int32_t block = 32;
    int32_t cols = 0;
    int32_t rows = 0;
    // Frame extent the grid was built for. The last row and column of blocks are
    // partial whenever it is not a multiple of `block`, and a rectangle derived
    // from the grid has to stop at the real edge rather than past it.
    int32_t width = 0;
    int32_t height = 0;
    std::vector<int8_t> deltas;
};

// 8-bit region-of-interest mask at any resolution: 0 is background, 255 is
// maximum quality. It is sampled onto the frame, so it need not match its size.
struct Mask {
    // A span, not a pointer: the caller states how many bytes there are, and this library
    // reads within them. With a bare pointer the only bound available was width x height
    // computed from fields the caller also supplied -- so a wrong height read past the end
    // of somebody else's buffer, and nothing on this side could tell.
    std::span<const uint8_t> data;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0; // bytes per row; 0 means tightly packed

    /// Whether the span actually holds the rows the dimensions claim. Checked before use;
    /// a mask that fails this is ignored rather than half-read.
    bool consistent() const
    {
        if (width <= 0 || height <= 0) {
            return false;
        }
        const std::size_t pitch =
            static_cast<std::size_t>(stride > 0 ? stride : width);
        return pitch >= static_cast<std::size_t>(width)
            && data.size() >= pitch * static_cast<std::size_t>(height);
    }
};

// How mask levels become QP deltas. Fewer levels means fewer distinct values,
// and therefore fewer rectangles to spend the encoder's region budget on.
struct MaskMapping {
    int32_t qp_delta_at_255 = -6; // strongest ROI
    int32_t qp_delta_at_0 = 6;    // background
    int32_t levels = 4;
};

// Samples `mask` onto the frame's block grid. Each block takes the *strongest*
// quality it covers, not the average: crushing a block that caught the edge of
// a plate costs more than spending bits on the background beside it.
QpDeltaMap qp_map_from_mask(const Mask& mask, const MaskMapping& mapping,
    int32_t frame_width, int32_t frame_height, int32_t block = 32);

// Merges equal-delta blocks into rectangles, strongest |delta| first because the
// lowest index wins on overlap. Blocks at delta 0 are left out: zero means "no
// change", which is the absence of a region rather than a region asking for
// nothing. Truncated to `max_regions` -- mfxExtEncoderROI takes at most 256 and
// a VAAPI driver usually far fewer -- and `dropped`, if given, reports the cut.
std::vector<Rect> rects_from_qp_map(
    const QpDeltaMap& map, int32_t max_regions, int32_t* dropped = nullptr);

} // namespace smartvideo
