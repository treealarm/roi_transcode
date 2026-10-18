#pragma once

// PixelLayout <-> AVPixelFormat, on the FFmpeg side of the line.
//
// The mapping lives here and nowhere else. That is the whole point of the library having
// its own enum: FFmpeg numbers its formats positionally in a list it appends to, so a
// public header that borrowed those numbers would be promising something another project's
// release notes get to break. Here it can only be wrong in one place.
//
// It sits under engine/common rather than core/ because everything that needs it is an
// FFmpeg-side file, and the core stays on the other side of that boundary. The name of a
// pixel format is a different question and lives in core: pixel_layout_name is public
// API (picture_store.h) and knows nothing about FFmpeg.

#include "ta_roienc/picture_store.h"

namespace ta_roienc {

/// AV_PIX_FMT_NONE for a format with no FFmpeg equivalent, including HardwareSurface --
/// which names a class of formats rather than one, so it cannot map back.
int to_av_pix_fmt(PixelLayout f);

PixelLayout from_av_pix_fmt(int f);

} // namespace ta_roienc
