#pragma once

// The buffer behind WrapPicture -- the caller's planes plus the handle that keeps
// them alive.
//
// LayerStore in the public header describes the same pixels and deliberately owns nothing:
// it is a view that has to outlive the submit call. This is the other half of that pair,
// and the difference is the whole point of PictureView::owner -- an encoder may hold a
// frame well past the submit that delivered it (lookahead, B-frames), so the thing keeping
// the pixels alive has to travel with the Picture rather than sit on the caller's stack.
//
// It wraps a LayerStore rather than reimplementing one: the plane bookkeeping and its
// bounds checks are the same question, and two answers to it would be two places to fix.
//
// Internal rather than public: an embedder gets one from WrapPicture and needs no name for
// it. It has a header of its own only because the FFmpeg interop has to recognise it --
// as_avframe hands libavcodec a frame that borrows these pixels, and it can only do that
// without copying if it can also hand over the owner. See engine/common/engine_common.cpp.

#include "ta_roienc/picture_store.h"
#include "ta_roienc/picture_view.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace ta_roienc {

class OwnedLayerStore final : public IPictureStore {
public:
    explicit OwnedLayerStore(const PictureView& view)
        : m_planes(plane_array(view))
        , m_view(view.format.width, view.format.height, view.format.pixel_layout,
              std::span<const LayerStore::Layer>(m_planes.data(), m_planes.size()))
        , m_owner(view.owner)
    {
    }

    MemoryKind memory() const override { return m_view.memory(); }
    int32_t width() const override { return m_view.width(); }
    int32_t height() const override { return m_view.height(); }
    PixelLayout format() const override { return m_view.format(); }
    LayerView plane(int32_t index) override { return m_view.plane(index); }

    /// The handle that keeps the pixels alive. Copied by anything that outlives this
    /// buffer -- which, for libavcodec, is the reference it takes on submit.
    const std::shared_ptr<const void>& owner() const { return m_owner; }

private:
    /// PictureLayer to LayerStore::Layer. const_cast because LayerView is writable and
    /// the encoder only reads it; the bytes stay the caller's, and nothing in this library
    /// writes through them.
    static std::array<LayerStore::Layer, 4> plane_array(const PictureView& view)
    {
        std::array<LayerStore::Layer, 4> out{};
        for (std::size_t i = 0; i < view.layer_count && i < 4; ++i) {
            const PictureLayer& p = view.planes[i];
            out[i] = LayerStore::Layer{
                std::span<uint8_t>(const_cast<uint8_t*>(p.data), p.size),
                p.stride,
            };
        }
        return out;
    }

    // Declared before m_view: it is what m_view's spans point at, so it has to be alive
    // before that constructor runs. Member order is the only thing enforcing that.
    std::array<LayerStore::Layer, 4> m_planes;
    LayerStore m_view;
    std::shared_ptr<const void> m_owner;
};

} // namespace ta_roienc
