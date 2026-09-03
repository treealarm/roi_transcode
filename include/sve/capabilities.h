#pragma once

// What a backend can actually do, asked of the backend rather than assumed.
//
// TZ v2.0 §6.4 / §7.2: on Rockchip the number of ROI regions and the QP API differ
// between SoCs, so anything that hardcodes them is wrong on some hardware. The same
// holds for the backends we already have -- QSV takes up to 256 regions through
// mfxExtEncoderROI while a VA-API driver usually takes far fewer, and libx264 has no
// region limit at all because it works from a per-macroblock offset map.

#include <cstdint>
#include <optional>

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

    /// A per-block importance hint rather than a quantiser offset -- Intel's emphasis map,
    /// and one of the fields §7.2 lists and §23 reports.
    ///
    /// False on every backend, and since 2026-09-01 that is a measured answer rather than a
    /// placeholder. It was written here as something mfxExtMBQP's neighbourhood would
    /// deliver once a native oneVPL backend existed. That backend exists and sends
    /// mfxExtMBQP, and the neighbourhood is not there: the word "emphasis" does not occur
    /// anywhere in the pinned oneVPL headers, and mfxExtMBQP's three modes are all
    /// quantisers -- a QP, a QP delta, or a per-block choice between the two. What that
    /// backend delivers is the block map, which is qp_map below. Reporting emphasis_map for
    /// it would be renaming a feature rather than having one.
    bool emphasis_map = false;

    /// Regions may change between frames without reopening the encoder.
    bool runtime_roi_update = false;

    /// Can carry user-data-unregistered SEI (§16).
    bool sei = false;

    /// Opens CBR+filler when RateControlPolicy::target_bitrate is set, instead of CQP/CRF.
    /// On CPU this is currently true for H.264 and false for H.265 (guarded for stability).
    /// Hardware backends that only honour ROI under CQP leave this false and keep the outer
    /// BitrateBudget loop.
    bool native_bitrate = false;

    /// HEVC Main10 -- ten bits a sample. Asked of the encoder rather than assumed: whether
    /// libx265 was built with a ten-bit path, and whether the driver exposes one, are
    /// properties of the machine this runs on. §48's AT-009 turns on this answer, and a
    /// SKIP without it would be a SKIP without evidence.
    bool main10 = false;

    /// Takes AV_PIX_FMT_P010 directly. False does not mean ten bits are unavailable -- it
    /// means the frame is converted first, which is why this is separate from main10.
    bool p010_input = false;

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

/// What an opened encoder actually settled on -- the other half of EncoderCapabilities.
///
/// Capabilities say what a backend could do before it opened; this says what it did. §38's
/// calibration profile prints `b_frames`, `lookahead`, `spatial_aq` and `temporal_aq`, and
/// until 2026-09-03 no interface asked for them, so the committed profiles left the four
/// fields out rather than write a plausible number nobody chose. A backend settles them
/// itself and not always the way the caller might assume -- libx264 forces adaptive
/// quantisation on regardless, because its per-macroblock offsets do nothing without it,
/// which is exactly the kind of thing a calibration profile exists to pin.
///
/// Every field is optional, and absent means "this backend cannot say" rather than "off".
/// A calibration that recorded a default where the answer was unknown would be pinning our
/// assumption instead of the encoder's behaviour, and the next driver release would move it
/// silently.
struct EffectiveSettings {
    /// Consecutive B-frames the encoder will use. 0 is a real answer (§15.1's primary
    /// profile asks for exactly that); nullopt means it was not asked or not answered.
    std::optional<int32_t> b_frames;

    /// Lookahead depth in frames, 0 when the encoder decides frame by frame.
    std::optional<int32_t> lookahead_frames;

    /// Quantiser varied within a frame by local detail.
    std::optional<bool> spatial_aq;

    /// Quantiser varied by how much later frames will refer to this one -- x264's mb-tree
    /// and x265's cu-tree are this.
    std::optional<bool> temporal_aq;
};

} // namespace sve
