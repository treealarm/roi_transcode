#pragma once

// FFmpeg interop.
//
// This is the one public header that names an FFmpeg type, and it is deliberately not
// included by ta_roienc.h. Everything else in include/ compiles with nothing but include/ on the
// path, which is what lets an embedder submit frames without libavutil; AVFrame is an
// opt-in adapter rather than the input interface, kept in one file a caller opts into:
//
//     #include "ta_roienc/ta_roienc.h"      // the library
//     #include "ta_roienc/picture.h"    // ... and AVFrame, if you have one
//
// What lives here is only what needs an AVFrame to be spelled. Picture and StampedPicture live in
// ta_roienc/stamped_picture.h: the frame a caller submits is an IPictureStore, and this header's job
// is turning an AVFrame into one. The raw-plane API lives in ta_roienc/picture_view.h, which this
// header includes for convenience.
//
// tests/unit/headers_isolated.cpp compiles the rest without FFmpeg, so a stray
// include of this header from a sibling stops the build rather than an embedder's.

#include "ta_roienc/encode.h"
#include "ta_roienc/picture_info.h"
#include "ta_roienc/source_format.h"
#include "ta_roienc/picture_view.h"
#include "ta_roienc/verdict.h"
#include "ta_roienc/stamped_picture.h"
#include "ta_roienc/stamp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

namespace ta_roienc {

/// An AVFrame as a Picture.
///
/// The two ways in are named after what they do to the caller's frame, because the
/// difference is the whole contract: Ref adds a reference and leaves the caller's frame
/// theirs to unref; Adopt takes the pointer and the caller must not touch it again. Both
/// end in the same place -- the reference released when the last Picture holding it goes --
/// so no caller writes av_frame_free.
class AvPictureBridge {
public:
    /// av_frame_ref onto a frame of our own. The caller keeps theirs. Returns an empty
    /// Picture if the reference could not be taken.
    static Picture Ref(const AVFrame* frame);

    /// Takes ownership of `frame`, which must not be used by the caller afterwards.
    static Picture Adopt(AVFrame* frame);
};

/// Submits an AVFrame.
///
/// Takes a reference through AvPictureBridge::Ref and calls SubmitPicture(StampedPicture).
/// Written as a free function rather than a member so that the encoder's own interface
/// names no FFmpeg type; a member would put AVFrame back in encode.h.
///
/// `frame` is borrowed for the length of the call. If the encoder keeps it -- lookahead,
/// B-frames -- it is holding its own reference, not the caller's.
Verdict SubmitAvPicture(VideoEncoder& encoder, const AVFrame* frame,
    const PictureInfo& metadata);

/// The encoder's hardware device, or null when it encodes on the CPU.
///
/// The typed half of VideoEncoder::HardwareDevice, here for the same reason
/// SubmitAvPicture is: this is the one header allowed to say AVBufferRef. Returns null rather
/// than reinterpreting a handle of another kind, so a backend not built on libavcodec cannot
/// have its own pointer read as FFmpeg's.
///
/// Borrowed, not owned: the reference belongs to the encoder and goes with it. A caller who
/// needs the device to outlive the encoder takes av_buffer_ref itself.
AVBufferRef* EncoderHardwareDevice(const VideoEncoder& encoder);

} // namespace ta_roienc
