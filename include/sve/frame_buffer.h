#pragma once

// Pixels the library did not allocate itself, behind an interface rather than a pointer.
//
// This is what a caller hands over when the frame is not simply planes in system memory:
// a surface the backend's factory gave out, or a buffer the caller's own pipeline owns.
// It exists so the public headers can describe such a frame without naming AVFrame -- an
// embedder implements this, or receives an implementation from CreateFrame, and never
// links libavutil to do either.
//
// Two ways it gets used, and both matter:
//
//   - CreateFrame returns one. Its memory lives in the encoder's own pool, so submitting
//     it copies nothing. plane() writes straight into that memory.
//   - A caller implements one over their own buffers. The backend reads plane() and
//     stride() and wraps them, which costs no copy either as long as the format matches.

#include "sve/memory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sve {

/// The pixel layouts this library takes on the way in.
///
/// Its own numbering, not FFmpeg's. Borrowing AVPixelFormat's values looked tempting -- one
/// less mapping -- but those values are positional in an enum FFmpeg appends to, so
/// pinning them here would be promising something another project's release notes get to
/// break. The mapping lives inside the library, where it can be wrong in one place.
///
/// Short on purpose. These are the formats an encoder here actually accepts from a caller;
/// anything else is a conversion, and a conversion is a copy the caller should be choosing
/// deliberately rather than discovering in a profile.
enum class PixelFormat : int32_t {
    Unknown = 0,
    /// Planar Y, U, V at 4:2:0. Three planes.
    YUV420P,
    /// Planar Y with interleaved UV at 4:2:0. Two planes, and what every hardware encoder
    /// here wants.
    NV12,
    YUV422P,
    YUV444P,
    /// Luma only.
    GRAY8,
    /// The pixels live on the device and are not addressable from the CPU. plane() is
    /// empty for these, which is the answer rather than an error.
    HardwareSurface,
};

const char* pixel_format_name(PixelFormat f);

/// One plane's writable bytes and its row stride, together.
///
/// `bytes` spans the whole plane, so `bytes.size()` is the limit and there is nothing to
/// guess. Rows are `stride` bytes apart, which is at least the row's width and often more:
/// hardware likes its rows aligned, and assuming otherwise is the second-oldest bug in
/// video code.
struct PlaneView {
    std::span<uint8_t> bytes;
    int32_t stride = 0;

    bool empty() const { return bytes.empty(); }

    /// One row, bounds-checked against the plane. Empty if `y` is past the end -- writing
    /// a row through this cannot leave the plane however wrong `y` is.
    std::span<uint8_t> row(int32_t y) const
    {
        if (y < 0 || stride <= 0) {
            return {};
        }
        const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        if (offset >= bytes.size()) {
            return {};
        }
        return bytes.subspan(offset, std::min<std::size_t>(stride, bytes.size() - offset));
    }
};

class IFrameBuffer {
public:
    virtual ~IFrameBuffer() = default;

    /// Where these pixels live. Decides whether plane() can return anything at all.
    virtual MemoryType memory() const = 0;

    virtual int32_t width() const = 0;
    virtual int32_t height() const = 0;

    virtual PixelFormat format() const = 0;

    /// One writable plane, with its length. Empty when the memory is not addressable from
    /// the CPU, when the index names a plane this format does not have, and after
    /// commit() -- so an empty view is a normal answer, not a failure to check for.
    ///
    /// A span rather than a pointer, and the stride travelling with it rather than beside
    /// it. The two mistakes this closes are the two that actually happen: writing past the
    /// end of a plane, because a bare uint8_t* carries no length; and pairing a pointer
    /// with the wrong row stride, because they used to be separate calls.
    virtual PlaneView plane(int32_t index) = 0;

    /// Says the caller has finished writing. Hardware memory mapped for the CPU has to be
    /// unmapped before the encoder reads it, and that is what this does -- calling it is
    /// not optional on a factory frame, and submitting without it is an error rather than
    /// a slow path. Harmless on system memory.
    virtual void commit() {}
};

/// Planes the caller already has, as an IFrameBuffer. Borrowed, not owned, and no copy:
/// this is a view, and it has to outlive the submit call that reads it.
///
/// It exists so that "I have pixels in memory" stays a one-liner after SubmitFrame started
/// taking an interface. Without it, the simplest case in the library would be the one that
/// asks the caller to write a class.
class PlaneBuffer final : public IFrameBuffer {
public:
    PlaneBuffer() = default;
    /// `size` is each plane's length in bytes. Asked for rather than derived: only the
    /// caller knows how much they actually allocated, and a length this library guessed
    /// from the format would be exactly the wrong thing to bound writes with.
    /// One plane per entry, each with its own length. The lengths are asked for rather
    /// than derived: only the caller knows how much they actually allocated, and a length
    /// this library guessed from the format is exactly the wrong thing to bound writes
    /// with.
    struct Plane {
        std::span<uint8_t> bytes;
        int32_t stride = 0;
    };

    PlaneBuffer(int32_t width, int32_t height, PixelFormat format,
        std::span<const Plane> planes)
        : m_width(width), m_height(height), m_format(format)
    {
        const std::size_t n = std::min<std::size_t>(planes.size(), 4);
        for (std::size_t i = 0; i < n; ++i) {
            m_plane[i] = planes[i];
        }
    }

    MemoryType memory() const override { return MemoryType::CPU; }
    int32_t width() const override { return m_width; }
    int32_t height() const override { return m_height; }
    PixelFormat format() const override { return m_format; }

    PlaneView plane(int32_t index) override
    {
        if (index < 0 || index >= 4) {
            return {};
        }
        const Plane& p = m_plane[static_cast<std::size_t>(index)];
        return PlaneView{p.bytes, p.stride};
    }

private:
    int32_t m_width = 0;
    int32_t m_height = 0;
    PixelFormat m_format = PixelFormat::Unknown;
    Plane m_plane[4];
};

} // namespace sve
