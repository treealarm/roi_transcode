#pragma once

// The PixelFormat mapping, internal to the library.

#include "smartvideo/frame_buffer.h"

namespace smartvideo {

/// AV_PIX_FMT_NONE for a format with no FFmpeg equivalent, including HardwareSurface --
/// which names a class of formats rather than one, so it cannot map back.
int to_av_pix_fmt(PixelFormat f);

PixelFormat from_av_pix_fmt(int f);

} // namespace smartvideo
