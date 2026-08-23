// The public headers, compiled with include/ and nothing else on the path.
//
// public_api_test cannot prove this on its own: it links smartvideo, and smartvideo
// publishes ${FFMPEG_INCLUDE_DIRS} as PUBLIC, so libavcodec is on that test's include path
// whether or not a public header asks for it. A header that started including
// <libavcodec/avcodec.h> would keep compiling there and nobody would notice until an
// embedder without FFmpeg tried to build against us.
//
// This translation unit has no library behind it and nothing to link. If it stops
// compiling, a public header grew a dependency it must not have -- which is exactly the
// property TZ v2.0 §7 and §8 rely on, and the reason BackendSession is opaque.

#include "smartvideo/smartvideo.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace {

// An embedder's own buffer, implemented with include/ and nothing else. This is the part
// of the surface that would be impossible if Frame carried a void* to an AVFrame: there
// would be nothing to implement, only a pointer to produce, and producing it would need
// FFmpeg. That it compiles here is the whole claim.
class CallerOwnedBuffer final : public smartvideo::IFrameBuffer {
public:
    smartvideo::MemoryType memory() const override { return smartvideo::MemoryType::CPU; }
    int32_t width() const override { return 64; }
    int32_t height() const override { return 64; }
    smartvideo::PixelFormat format() const override
    {
        return smartvideo::PixelFormat::GRAY8;
    }
    smartvideo::PlaneView plane(int32_t index) override
    {
        if (index != 0) {
            return {};
        }
        return smartvideo::PlaneView{std::span<uint8_t>(m_pixels, sizeof(m_pixels)), 64};
    }

private:
    uint8_t m_pixels[64 * 64] = {};
};

// Naming the types is the test: each one has to be complete, and completing it drags in
// whatever its header includes.
[[maybe_unused]] void the_public_vocabulary_is_self_contained()
{
    smartvideo::Settings settings;
    smartvideo::SessionParams params;
    smartvideo::Report report;
    smartvideo::StreamInfo info;
    smartvideo::EncodedPacket packet;
    smartvideo::Rect rect;
    smartvideo::Box box;
    smartvideo::Mask mask;
    smartvideo::Rational rational;

    // §7: the backend contract, opaque session and all.
    smartvideo::EncoderConfig config;
    smartvideo::BackendFrameControl control;
    smartvideo::EncoderCapabilities caps;
    const smartvideo::Status status = smartvideo::Status::Ok();
    const std::span<const smartvideo::BackendRef> backend =
        smartvideo::backend_chain(smartvideo::Encoder::Cpu);

    // §8 and §10: memory types, objects, policies.
    const smartvideo::MemoryType memory = smartvideo::MemoryType::ONEVPL;
    smartvideo::Object object;
    smartvideo::QualityPolicy policy = smartvideo::QualityPolicy::OBJECT_FIRST;

    CallerOwnedBuffer caller_buffer;
    caller_buffer.commit();

    // Planes the caller already has, without writing a class for them.
    uint8_t pixels[16 * 16] = {};
    const smartvideo::PlaneBuffer::Plane described[4] = {
        {std::span<uint8_t>(pixels, sizeof(pixels)), 16}, {}, {}, {}};
    smartvideo::PlaneBuffer plain(16, 16, smartvideo::PixelFormat::GRAY8,
        std::span<const smartvideo::PlaneBuffer::Plane>(described, 4));
    // A row past the end comes back empty rather than pointing somewhere.
    (void)plain.plane(0).row(999).size();
    (void)smartvideo::pixel_format_name(smartvideo::PixelFormat::NV12);

    (void)settings; (void)params; (void)report; (void)info; (void)packet;
    (void)rect; (void)box; (void)mask; (void)rational; (void)config;
    (void)control; (void)caps; (void)status; (void)backend; (void)memory;

    // The owning types are named, and the caller never writes a delete.
    smartvideo::FrameBufferPtr owned_buffer;
    smartvideo::SessionPtr owned_session;

    // And unique ownership is not a ceiling. A caller who needs a frame in a map, or alive
    // across threads, converts -- the deleter travels with it, so the library still frees
    // it with the allocator that made it. This compiles, which is the point: returning
    // unique_ptr from the factory leaves that choice open, and returning shared_ptr would
    // have charged every caller an atomic refcount for a choice most of them never make.
    std::shared_ptr<smartvideo::IFrameBuffer> shared_buffer = std::move(owned_buffer);
    (void)shared_buffer; (void)owned_session;

    (void)object; (void)policy; (void)caller_buffer;
}

} // namespace
