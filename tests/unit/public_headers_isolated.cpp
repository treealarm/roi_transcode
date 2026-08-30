// The public headers, compiled with include/ and nothing else on the path.
//
// public_api_test cannot prove this on its own: it links sve, and sve
// publishes ${FFMPEG_INCLUDE_DIRS} as PUBLIC, so libavcodec is on that test's include path
// whether or not a public header asks for it. A header that started including
// <libavcodec/avcodec.h> would keep compiling there and nobody would notice until an
// embedder without FFmpeg tried to build against us.
//
// This translation unit has no library behind it and nothing to link. If it stops
// compiling, a public header grew a dependency it must not have -- which is exactly the
// property TZ v2.0 §7 and §8 rely on, and the reason BackendSession is opaque.

#include "sve/input_format.h"
#include "sve/packet.h"
#include "sve/sve.h"
#include "sve/timestamp.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace {

// An embedder's own buffer, implemented with include/ and nothing else. This is the part
// of the surface that would be impossible if Frame carried a void* to an AVFrame: there
// would be nothing to implement, only a pointer to produce, and producing it would need
// FFmpeg. That it compiles here is the whole claim.
class CallerOwnedBuffer final : public sve::IFrameBuffer {
public:
    sve::MemoryType memory() const override { return sve::MemoryType::CPU; }
    int32_t width() const override { return 64; }
    int32_t height() const override { return 64; }
    sve::PixelFormat format() const override
    {
        return sve::PixelFormat::GRAY8;
    }
    sve::PlaneView plane(int32_t index) override
    {
        if (index != 0) {
            return {};
        }
        return sve::PlaneView{std::span<uint8_t>(m_pixels, sizeof(m_pixels)), 64};
    }

private:
    uint8_t m_pixels[64 * 64] = {};
};

// Naming the types is the test: each one has to be complete, and completing it drags in
// whatever its header includes.
[[maybe_unused]] void the_public_vocabulary_is_self_contained()
{
    sve::Settings settings;
    sve::SessionParams params;
    sve::Report report;
    sve::StreamInfo info;
    sve::EncodedPacket packet;
    sve::Rect rect;
    sve::Box box;
    sve::Mask mask;
    sve::Rational rational;

    // §8.3, §8.6 and §8.7. These three are named here rather than only in frame_test
    // because the claim being made is not that they work -- it is that they compile with
    // no FFmpeg anywhere. frame.h is deliberately absent: it is the one public header that
    // does name an AVFrame, and it is not reachable from sve.h.
    sve::InputVideoFormat input_format;
    sve::MediaTimestamp timestamp;
    sve::PacketBuffer payload;
    (void)input_format.valid();
    (void)timestamp.valid();
    (void)payload.empty();

    // §7: the backend contract, opaque session and all.
    sve::EncoderConfig config;
    sve::BackendFrameControl control;
    sve::EncoderCapabilities caps;
    const sve::Status status = sve::Status::Ok();
    const std::span<const sve::BackendRef> backend =
        sve::backend_chain(sve::Encoder::Cpu);

    // §8 and §10: memory types, objects, policies.
    const sve::MemoryType memory = sve::MemoryType::ONEVPL;
    sve::Object object;
    sve::QualityPolicy policy = sve::QualityPolicy::OBJECT_FIRST;

    CallerOwnedBuffer caller_buffer;
    caller_buffer.commit();

    // Planes the caller already has, without writing a class for them.
    uint8_t pixels[16 * 16] = {};
    const sve::PlaneBuffer::Plane described[4] = {
        {std::span<uint8_t>(pixels, sizeof(pixels)), 16}, {}, {}, {}};
    sve::PlaneBuffer plain(16, 16, sve::PixelFormat::GRAY8,
        std::span<const sve::PlaneBuffer::Plane>(described, 4));
    // A row past the end comes back empty rather than pointing somewhere.
    (void)plain.plane(0).row(999).size();
    (void)sve::pixel_format_name(sve::PixelFormat::NV12);

    (void)settings; (void)params; (void)report; (void)info; (void)packet;
    (void)rect; (void)box; (void)mask; (void)rational; (void)config;
    (void)input_format; (void)timestamp; (void)payload;
    (void)control; (void)caps; (void)status; (void)backend; (void)memory;

    // The owning types are named, and the caller never writes a delete.
    sve::FrameBufferPtr owned_buffer;
    sve::SessionPtr owned_session;

    // And unique ownership is not a ceiling. A caller who needs a frame in a map, or alive
    // across threads, converts -- the deleter travels with it, so the library still frees
    // it with the allocator that made it. This compiles, which is the point: returning
    // unique_ptr from the factory leaves that choice open, and returning shared_ptr would
    // have charged every caller an atomic refcount for a choice most of them never make.
    std::shared_ptr<sve::IFrameBuffer> shared_buffer = std::move(owned_buffer);
    (void)shared_buffer; (void)owned_session;

    (void)object; (void)policy; (void)caller_buffer;
}

} // namespace
