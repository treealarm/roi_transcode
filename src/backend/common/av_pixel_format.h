#pragma once

// PixelFormat <-> AVPixelFormat, on the FFmpeg side of the line.
//
// The mapping lives here and nowhere else. That is the whole point of the library having
// its own enum: FFmpeg numbers its formats positionally in a list it appends to, so a
// public header that borrowed those numbers would be promising something another project's
// release notes get to break. Here it can only be wrong in one place.
//
// It sits under backend/common rather than core/ because everything that needs it is an
// FFmpeg-side file, and §5.5 puts the core on the other side of that boundary. The name of
// a pixel format is a different question and stayed in core: pixel_format_name is public
// API (frame_buffer.h) and knows nothing about FFmpeg.

#include "sve/frame_buffer.h"

namespace sve {

/// AV_PIX_FMT_NONE for a format with no FFmpeg equivalent, including HardwareSurface --
/// which names a class of formats rather than one, so it cannot map back.
int to_av_pix_fmt(PixelFormat f);

PixelFormat from_av_pix_fmt(int f);

} // namespace sve
