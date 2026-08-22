#pragma once

// What a backend can actually do, asked of the backend rather than assumed.
//
// TZ v2.0 §6.4 gives these fields, and gives the reason: on Rockchip the number of ROI
// regions and the QP API differ between SoCs, so anything that hardcodes them is wrong on
// some hardware. The same turns out to hold for the backends we already have -- QSV takes
// up to 256 regions through mfxExtEncoderROI while a VA-API driver usually takes far fewer,
// and libx264 has no region limit at all because it works from a per-macroblock offset map.

#include <cstdint>

namespace smartvideo {

struct EncoderCapabilities {
    bool h264 = false;
    bool h265 = false;

    /// Rectangular regions with a QP offset each -- what AVRegionOfInterest maps onto.
    bool roi_rectangles = false;
    /// A QP offset per coding block, rather than per rectangle.
    bool per_block_qp = false;
    /// H.265 CTU-level QP specifically (§6.2).
    bool ctu_qp = false;
    /// Regions may change between frames without reopening the encoder.
    bool runtime_roi_update = false;
    /// Can carry user-data-unregistered SEI (§16).
    bool sei = false;

    /// Upper bound on regions the backend accepts. 0 means "no limit it can name".
    /// Requests above it are truncated strongest-first and the loss is reported.
    uint32_t max_roi_regions = 0;
};

} // namespace smartvideo
