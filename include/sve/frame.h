#pragma once

// FFmpeg interop -- TZ v4.1 §8.5.
//
// This is the one public header that names an FFmpeg type, and it is deliberately not
// included by sve.h. Everything else in include/ compiles with nothing but include/ on the
// path, which is what lets an embedder submit frames without libavutil; §20 makes AVFrame
// an opt-in adapter rather than the input interface, so the two requirements are met by
// keeping the interop in one file a caller opts into:
//
//     #include "sve/sve.h"      // the library
//     #include "sve/frame.h"    // ... and AVFrame, if you have one
//
// What lives here is only what needs an AVFrame to be spelled. Frame and TimedFrame moved
// to sve/timed_frame.h in 4.1 and are in the umbrella now: the frame a caller submits is
// an IFrameBuffer, and this header's job is turning an AVFrame into one. The raw-plane API
// of §8.10 moved out for the same reason and now lives in sve/raw_frame.h, which this
// header includes so that callers who had both from here still do.
//
// tests/unit/public_headers_isolated.cpp compiles the rest without FFmpeg, so a stray
// include of this header from a sibling stops the build rather than an embedder's.

#include "sve/encoder.h"
#include "sve/frame_metadata.h"
#include "sve/input_format.h"
#include "sve/raw_frame.h"
#include "sve/status.h"
#include "sve/timed_frame.h"
#include "sve/timestamp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

namespace sve {

/// An AVFrame as a Frame -- TZ v4.1 §8.5.
///
/// The two ways in are named after what they do to the caller's frame, because the
/// difference is the whole contract: Ref adds a reference and leaves the caller's frame
/// theirs to unref; Adopt takes the pointer and the caller must not touch it again. Both
/// end in the same place -- the reference released when the last Frame holding it goes --
/// so no caller writes av_frame_free.
class AvFrameAdapter {
public:
    /// av_frame_ref onto a frame of our own. The caller keeps theirs. Returns an empty
    /// Frame if the reference could not be taken.
    static Frame Ref(const AVFrame* frame);

    /// Takes ownership of `frame`, which must not be used by the caller afterwards.
    static Frame Adopt(AVFrame* frame);
};

/// Submits an AVFrame -- TZ v4.1 §8.1 and §8.5.
///
/// The adapter §8.1 asks for and nothing more: it takes a reference through
/// AvFrameAdapter::Ref and calls SubmitFrame(TimedFrame). Written as a free function
/// rather than a member so that the encoder's own interface names no FFmpeg type; a member
/// would put AVFrame back in encoder.h, which is exactly what §8.3 forbids.
///
/// `frame` is borrowed for the length of the call. If the encoder keeps it -- lookahead,
/// B-frames -- it is holding its own reference, not the caller's.
Status SubmitAvFrame(SmartVideoEncoder& encoder, const AVFrame* frame,
    const FrameMetadata& metadata);

} // namespace sve
