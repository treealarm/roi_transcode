#pragma once

// How many ROI regions the Intel driver will actually take, per codec.
//
// Both Intel backends encode through the same iHD driver, so the number is a property of
// the driver rather than of either API in front of it -- which is why it lives here rather
// than twice, once in each backend, drifting apart.
//
// It is not the capacity of the structure that carries the regions. mfxExtEncoderROI holds
// 256; what runs out first is a fixed-size array inside iHD's VDEnc path, and its size
// differs between the two codecs.
//
// The two APIs fail differently when it is exceeded, which is why this was found late:
//
//   * VA-API: libavcodec queries VAConfigAttribEncROI and trims the list itself, so nothing
//     crashes -- the regions past the limit are simply dropped, silently, somewhere our
//     regions_dropped counter cannot see them. Declaring the real number here moves that
//     truncation back to our side, where it is counted and reported (TZ v3.4 §50 forbids a
//     silent fallback, and a region quietly discarded by a dependency is one).
//   * oneVPL: libmfx-gen forwards every region as given. iHD overruns its array and the
//     process dies inside vaEndPicture on __stack_chk_fail -- no bad frame, no error
//     status, in a thread belonging to the runtime rather than to us.
//
// Measured on Alder Lake with iHD 25.2, on a 960x540 clip carrying six tracked regions:
// H.264 encodes cleanly at four and aborts at five; H.265 takes six without complaint.
// Four and eight are also the VDEnc AVC and HEVC region counts in the driver, so the
// boundary is the driver's own and not a property of that clip.

#include "sve/types.h"

#include <cstdint>

namespace sve {

/// Regions the iHD driver accepts for `codec`. Requests above it are truncated
/// strongest-first by the caller and counted.
constexpr uint32_t intel_max_roi_regions(Codec codec)
{
    return codec == Codec::H264 ? 4 : 8;
}

} // namespace sve
