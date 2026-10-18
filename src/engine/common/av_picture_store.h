#pragma once

// IPictureStore over an AVFrame -- the implementation that never leaves this library.
//
// Two jobs. It is what the decoder's output and the probe's scratch frame are handed to
// SubmitPicture as, so the internal paths use the same door an embedder does rather than a
// private one beside it. And it is what CreatePicture returns, which is why it can own the
// frame as well as borrow it.

#include "av_handles.h"
#include "engine/common/av_pixel_layout.h"
#include "ta_roienc/picture_store.h"

#include <cstddef>
#include <span>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace ta_roienc {

class AvPictureStore final : public IPictureStore {
public:
    /// Borrows a frame someone else owns. The common case inside the library: the decoder
    /// hands out a frame it will reuse, and wrapping it must not take ownership of it.
    explicit AvPictureStore(AVFrame* borrowed, MemoryKind memory = MemoryKind::AVFRAME)
        : m_frame(borrowed), m_memory(memory)
    {
    }

    ~AvPictureStore() override
    {
        av_frame_free(&m_mapped);
        if (m_owned) {
            av_frame_free(&m_frame);
        }
    }

    AvPictureStore(const AvPictureStore&) = delete;
    AvPictureStore& operator=(const AvPictureStore&) = delete;

    MemoryKind memory() const override { return m_memory; }
    int32_t width() const override { return m_frame ? m_frame->width : 0; }
    int32_t height() const override { return m_frame ? m_frame->height : 0; }
    PixelLayout format() const override
    {
        return m_frame ? from_av_pix_fmt(m_frame->format) : PixelLayout::Unknown;
    }

    LayerView plane(int32_t index) override
    {
        if (index < 0 || index >= AV_NUM_DATA_POINTERS) {
            return {};
        }
        // The mapping is what the caller writes into when the pixels live on the GPU;
        // without one, the frame's own planes are the answer.
        AVFrame* w = m_mapped ? m_mapped : m_frame;
        if (!w || !w->data[index] || w->linesize[index] <= 0) {
            return {};
        }
        return LayerView{
            std::span<uint8_t>(w->data[index], plane_bytes(w, index)), w->linesize[index]};
    }

    void commit() override
    {
        // Unmapping is what publishes the writes to the surface the encoder will read.
        // Leaving it mapped is not a slow path, it is a wrong picture.
        if (m_mapped) {
            av_frame_free(&m_mapped);
        }
    }

    /// The frame itself, for the backend that is about to encode it. Internal: an embedder
    /// reaches the pixels through plane() and never needs this.
    AVFrame* frame() const { return m_frame; }
    bool committed() const { return m_mapped == nullptr; }

    /// Takes ownership of a frame this buffer allocated, with an optional CPU-writable
    /// view of it. Used by CreatePicture.
    void adopt(AVFrame* owned, AVFrame* mapped, MemoryKind memory)
    {
        av_frame_free(&m_mapped);
        if (m_owned) {
            av_frame_free(&m_frame);
        }
        m_frame = owned;
        m_mapped = mapped;
        m_memory = memory;
        m_owned = true;
    }

private:
    /// How many bytes a plane holds: its stride times its own height. Chroma planes are
    /// shorter than the frame by the format's vertical subsampling -- half for NV12 and
    /// YUV420P -- and calling them full height is how a bound ends up too generous to be
    /// worth having.
    static std::size_t plane_bytes(const AVFrame* f, int32_t index)
    {
        int rows = f->height;
        if (index > 0) {
            const AVPixFmtDescriptor* desc =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
            const int shift = desc ? desc->log2_chroma_h : 0;
            rows = (f->height + (1 << shift) - 1) >> shift;
        }
        return static_cast<std::size_t>(f->linesize[index]) * static_cast<std::size_t>(rows);
    }

    AVFrame* m_frame = nullptr;
    AVFrame* m_mapped = nullptr;
    MemoryKind m_memory = MemoryKind::AVFRAME;
    bool m_owned = false;
};

} // namespace ta_roienc
