#pragma once

// Where a frame's pixels live.
//
// CPU and AVFRAME are what a caller can hand the encoder. VAAPI and ONEVPL are the device
// surfaces a hardware backend's CreateFrame hands out from its own pool.

namespace ta_roienc {

enum class MemoryType {
    /// Tightly packed planes in system memory. Accepted.
    CPU,
    /// An AVFrame the caller owns, hardware or software. Accepted.
    AVFRAME,
    /// VASurfaceID. The VA-API backend uploads software frames itself today.
    VAAPI,
    /// mfxFrameSurface1. The oneVPL backend uploads software frames itself today.
    ONEVPL,
};

/// Whether a caller can hand the encoder pixels in this form. Device surfaces come from a
/// backend's own CreateFrame rather than from the caller, and the caller learns that before
/// submitting a frame rather than after.
constexpr bool memory_type_supported(MemoryType t)
{
    return t == MemoryType::CPU || t == MemoryType::AVFRAME;
}

const char* memory_type_name(MemoryType t);

} // namespace ta_roienc
