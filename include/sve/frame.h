#pragma once

// Owning a frame, and borrowing one -- TZ v3.4 §8.5 and §8.10.
//
// This is the one public header that names an FFmpeg type, and it is deliberately not
// included by sve.h. Everything else in include/ compiles with nothing but include/ on the
// path, which is what lets an embedder write a PacketHandler without libavutil; §55 makes
// AVFrame a mandatory input interface, so the two requirements are met by keeping the
// interop in one file a caller opts into:
//
//     #include "sve/sve.h"      // the library
//     #include "sve/frame.h"    // ... and AVFrame, if you have one
//
// tests/unit/public_headers_isolated.cpp compiles the rest without FFmpeg, so a stray
// include of this header from a sibling stops the build rather than an embedder's.

#include "sve/input_format.h"
#include "sve/status.h"
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

/// A frame this object keeps alive.
///
/// The two ways to get one are named after what they do to the caller's frame, because
/// the difference is the whole contract: Ref adds a reference and leaves the caller's
/// frame theirs to unref; Adopt takes the pointer and the caller must not touch it again.
/// Both end in the same place -- av_frame_free on destruction -- so no caller writes one.
class Frame {
public:
    Frame() = default;

    /// av_frame_ref onto a new frame. The caller keeps theirs. Returns an empty Frame if
    /// the reference could not be taken.
    static Frame Ref(const AVFrame* frame);

    /// Takes ownership of `frame`, which must not be used by the caller afterwards.
    static Frame Adopt(AVFrame* frame);

    const AVFrame* get() const noexcept { return m_frame.get(); }
    AVFrame* mutable_get() noexcept { return m_frame.get(); }

    explicit operator bool() const noexcept { return m_frame != nullptr; }

private:
    /// The free happens inside the library. On Windows a DLL and its caller do not share
    /// an allocator, and av_frame_free inlined into the caller's translation unit would
    /// free on the wrong side of that line -- the same reason SessionDeleter exists.
    struct Deleter {
        void operator()(AVFrame* frame) const noexcept;
    };

    explicit Frame(AVFrame* frame) : m_frame(frame) {}

    std::unique_ptr<AVFrame, Deleter> m_frame;
};

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
    MediaTimestamp timestamp;
    std::shared_ptr<const void> owner;
};

/// Wraps `view` into `output` without copying pixels. Validates the format, the plane
/// count for that format, and that every plane holds the rows its stride and the frame
/// height claim -- so a short buffer is an InvalidArgument here rather than a read past
/// the end inside an encoder.
Status WrapRawFrame(const RawFrameView& view, Frame* output);

} // namespace sve
