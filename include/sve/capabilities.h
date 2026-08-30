#pragma once

// What a backend can actually do, asked of the backend rather than assumed.
//
// TZ v2.0 §6.4 / §7.2: on Rockchip the number of ROI regions and the QP API differ
// between SoCs, so anything that hardcodes them is wrong on some hardware. The same
// holds for the backends we already have -- QSV takes up to 256 regions through
// mfxExtEncoderROI while a VA-API driver usually takes far fewer, and libx264 has no
// region limit at all because it works from a per-macroblock offset map.

#include <cstdint>

namespace sve {

struct EncoderCapabilities {
    bool h264 = false;
    bool h265 = false;

    /// Rectangular regions with a QP offset each -- what AVRegionOfInterest maps onto.
    bool roi_rectangles = false;

    /// A QP offset per coding block (macroblock / CTU), rather than only per rectangle.
    /// When true, the adaptive layer may keep a full block grid and must not pretend the
    /// hardware's rectangle budget is the limit -- FFmpeg still delivers the map through
    /// ROI side data, which libx264/libx265 expand into quantOffsets.
    bool qp_map = false;

    /// Cell size of that grid when qp_map is true; both zero when it is not.
    /// H.264 macroblocks are 16x16; HEVC CTUs here are treated as 32x32 (the size this
    /// library paints before merge -- the encoder may use a larger CTU internally).
    uint32_t qp_map_block_width = 0;
    uint32_t qp_map_block_height = 0;

    /// H.265 CTU-level QP specifically (§6.2). Distinct from qp_map only if a backend
    /// exposes CTU control without a general block map; none do yet.
    bool ctu_qp = false;

    /// Regions may change between frames without reopening the encoder.
    bool runtime_roi_update = false;

    /// Can carry user-data-unregistered SEI (§16).
    bool sei = false;

    /// Opens CBR+filler when RateControlPolicy::target_bitrate is set, instead of CQP/CRF.
    /// On CPU this is currently true for H.264 and false for H.265 (guarded for stability).
    /// Hardware backends that only honour ROI under CQP leave this false and keep the outer
    /// BitrateBudget loop.
    bool native_bitrate = false;

    /// Upper bound on regions the backend accepts. 0 means "no limit it can name".
    /// Requests above it are truncated strongest-first and the loss is reported.
    /// Ignored for truncation when qp_map is true: the map is the interface then.
    uint32_t max_roi_regions = 0;
};

/// Edge length of a square qp_map cell, or 0 if the backend has no qp_map.
inline int32_t qp_map_block(const EncoderCapabilities& c)
{
    if (!c.qp_map) {
        return 0;
    }
    if (c.qp_map_block_width > 0) {
        return static_cast<int32_t>(c.qp_map_block_width);
    }
    return static_cast<int32_t>(c.qp_map_block_height);
}

} // namespace sve
