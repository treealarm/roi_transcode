#pragma once

// The buffer behind WrapRawFrame (§8.10) -- the caller's planes plus the handle that keeps
// them alive.
//
// PlaneBuffer in the public header describes the same pixels and deliberately owns nothing:
// it is a view that has to outlive the submit call. This is the other half of that pair,
// and the difference is the whole point of RawFrameView::owner -- an encoder may hold a
// frame well past the submit that delivered it (lookahead, B-frames), so the thing keeping
// the pixels alive has to travel with the Frame rather than sit on the caller's stack.
//
// It wraps a PlaneBuffer rather than reimplementing one: the plane bookkeeping and its
// bounds checks are the same question, and two answers to it would be two places to fix.
//
// Internal rather than public: an embedder gets one from WrapRawFrame and needs no name for
// it. It has a header of its own only because the FFmpeg interop has to recognise it --
// as_avframe hands libavcodec a frame that borrows these pixels, and it can only do that
// without copying if it can also hand over the owner. See backend/common/backend_common.cpp.

#include "sve/frame_buffer.h"
#include "sve/raw_frame.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace sve {

class OwnedPlaneBuffer final : public IFrameBuffer {
public:
    explicit OwnedPlaneBuffer(const RawFrameView& view)
        : m_planes(plane_array(view))
        , m_view(view.format.width, view.format.height, view.format.pixel_format,
              std::span<const PlaneBuffer::Plane>(m_planes.data(), m_planes.size()))
        , m_owner(view.owner)
    {
    }

    MemoryType memory() const override { return m_view.memory(); }
    int32_t width() const override { return m_view.width(); }
    int32_t height() const override { return m_view.height(); }
    PixelFormat format() const override { return m_view.format(); }
    PlaneView plane(int32_t index) override { return m_view.plane(index); }

    /// The handle that keeps the pixels alive. Copied by anything that outlives this
    /// buffer -- which, for libavcodec, is the reference it takes on submit.
    const std::shared_ptr<const void>& owner() const { return m_owner; }

private:
    /// RawFramePlane to PlaneBuffer::Plane. const_cast because PlaneView is writable and
    /// the encoder only reads it; the bytes stay the caller's, and nothing in this library
    /// writes through them.
    static std::array<PlaneBuffer::Plane, 4> plane_array(const RawFrameView& view)
    {
        std::array<PlaneBuffer::Plane, 4> out{};
        for (std::size_t i = 0; i < view.plane_count && i < 4; ++i) {
            const RawFramePlane& p = view.planes[i];
            out[i] = PlaneBuffer::Plane{
                std::span<uint8_t>(const_cast<uint8_t*>(p.data), p.size),
                p.stride,
            };
        }
        return out;
    }

    // Declared before m_view: it is what m_view's spans point at, so it has to be alive
    // before that constructor runs. Member order is the only thing enforcing that.
    std::array<PlaneBuffer::Plane, 4> m_planes;
    PlaneBuffer m_view;
    std::shared_ptr<const void> m_owner;
};

} // namespace sve
