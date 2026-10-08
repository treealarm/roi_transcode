#pragma once

// FFmpeg interop.
//
// This is the one public header that names an FFmpeg type, and it is deliberately not
// included by sve.h. Everything else in include/ compiles with nothing but include/ on the
// path, which is what lets an embedder submit frames without libavutil; AVFrame is an
// opt-in adapter rather than the input interface, kept in one file a caller opts into:
//
//     #include "sve/sve.h"      // the library
//     #include "sve/frame.h"    // ... and AVFrame, if you have one
//
// What lives here is only what needs an AVFrame to be spelled. Frame and TimedFrame live in
// sve/timed_frame.h: the frame a caller submits is an IFrameBuffer, and this header's job
// is turning an AVFrame into one. The raw-plane API lives in sve/raw_frame.h, which this
// header includes for convenience.
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

/// An AVFrame as a Frame.
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

/// Submits an AVFrame.
///
/// Takes a reference through AvFrameAdapter::Ref and calls SubmitFrame(TimedFrame).
/// Written as a free function rather than a member so that the encoder's own interface
/// names no FFmpeg type; a member would put AVFrame back in encoder.h.
///
/// `frame` is borrowed for the length of the call. If the encoder keeps it -- lookahead,
/// B-frames -- it is holding its own reference, not the caller's.
Status SubmitAvFrame(SmartVideoEncoder& encoder, const AVFrame* frame,
    const FrameMetadata& metadata);

/// The encoder's hardware device, or null when it encodes on the CPU.
///
/// The typed half of SmartVideoEncoder::HardwareDevice, here for the same reason
/// SubmitAvFrame is: this is the one header allowed to say AVBufferRef. Returns null rather
/// than reinterpreting a handle of another kind, so a backend not built on libavcodec cannot
/// have its own pointer read as FFmpeg's.
///
/// Borrowed, not owned: the reference belongs to the encoder and goes with it. A caller who
/// needs the device to outlive the encoder takes av_buffer_ref itself.
AVBufferRef* EncoderHardwareDevice(const SmartVideoEncoder& encoder);

} // namespace sve
