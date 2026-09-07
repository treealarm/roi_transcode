#pragma once

// The FFmpeg handle inside a DecodeDevice, for this library's own decode sources.
//
// It is a friend rather than a public accessor so that sve/decoder.h stays free of
// libavutil: a public method returning AVBufferRef* would put FFmpeg back into a header an
// embedder compiles, which is what §8.3 keeps out. Everything that needs the handle is on
// this side of the boundary.

#include "sve/decoder.h"

extern "C" {
#include <libavutil/buffer.h>
}

namespace sve {

struct DecodeDeviceAccess {
    /// Borrowed, not owned: the reference belongs to the device. Null for the CPU device,
    /// which is what FrameDecoder::open already reads as "decode in software".
    static AVBufferRef* device(const DecodeDevice& d) noexcept;
};

} // namespace sve
