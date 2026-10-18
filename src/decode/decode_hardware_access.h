#pragma once

// The FFmpeg handle inside a DecodeDevice, for this library's own decode sources.
//
// It is a friend rather than a public accessor so that ta_roienc/decoder.h stays free of
// libavutil: a public method returning AVBufferRef* would put FFmpeg back into a header an
// embedder compiles. Everything that needs the handle is on this side of the boundary.

#include "ta_roienc/decoder.h"

extern "C" {
#include <libavutil/buffer.h>
}

namespace ta_roienc {

struct DecodeDeviceAccess {
    /// Borrowed, not owned: the reference belongs to the device. Null for the CPU device,
    /// which is what PictureDecoder::open already reads as "decode in software".
    static AVBufferRef* device(const DecodeDevice& d) noexcept;
};

} // namespace ta_roienc
