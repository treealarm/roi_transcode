#pragma once

// Raw planes API -- TZ v4.1 §8.10.
//
// Pixels a caller already has, wrapped into a Frame without copying. This used to live in
// sve/frame.h, which is the one public header that names an FFmpeg type, and that placement
// was wrong on its own terms: nothing about "here are my planes" needs libavutil, yet an
// embedder who wanted §8.10 had to include §8.5's FFmpeg interop to reach it. §5.5 makes
// the point structural -- the core depends on no FFmpeg -- so the declaration moved here
// and the implementation stopped going through AVFrame.
//
// sve/frame.h still includes this header, so a caller who was including it keeps compiling.

#include "sve/frame_buffer.h"
#include "sve/input_format.h"
#include "sve/status.h"
#include "sve/timed_frame.h"
#include "sve/timestamp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace sve {

/// One plane of pixels the caller owns, with the length that bounds it. A pointer alone
/// carries no length, and a stride passed separately from the pointer it belongs to is the
/// oldest bug in video code.
struct RawFramePlane {
    const uint8_t* data = nullptr;
    int32_t stride = 0;
    std::size_t size = 0;
};

/// Pixels the caller has somewhere else, described well enough to wrap without copying.
///
/// `owner` is the part that makes this safe and the part a caller most wants to skip. It
/// keeps the pixels alive for as long as the resulting Frame exists -- WrapRawFrame holds
/// it inside the frame, so this is enforced rather than merely asked for. A view whose
/// owner is empty is refused rather than wrapped: the alternative is a Frame pointing into
/// a buffer that was freed while the encoder still held it. A caller who cannot make that
/// promise passes a copy instead.
struct RawFrameView {
    InputVideoFormat format;
    std::array<RawFramePlane, 4> planes{};
    std::size_t plane_count = 0;
    /// Carried for the caller's convenience, not read by the library: the timestamp an
    /// encoder uses comes from FrameMetadata, which is what SubmitFrame takes beside the
    /// frame. It used to be written into the AVFrame this function built, and even then
    /// the backend overwrote it from FrameMetadata one call later -- so a view whose
    /// timestamp disagrees with the metadata has always lost that argument.
    MediaTimestamp timestamp;
    std::shared_ptr<const void> owner;
};

/// Wraps `view` into `output` without copying pixels. Validates the format, the plane
/// count for that format, and that every plane holds the rows its stride and the frame
/// height claim -- so a short buffer is an InvalidArgument here rather than a read past
/// the end inside an encoder.
Status WrapRawFrame(const RawFrameView& view, Frame* output);

} // namespace sve
