// The public headers, compiled with include/ and nothing else on the path.
//
// public_api_test cannot prove this on its own: it links ta_roienc, and ta_roienc
// publishes ${FFMPEG_INCLUDE_DIRS} as PUBLIC, so libavcodec is on that test's include path
// whether or not a public header asks for it. A header that started including
// <libavcodec/avcodec.h> would keep compiling there and nobody would notice until an
// embedder without FFmpeg tried to build against us.
//
// This translation unit has no library behind it and nothing to link. If it stops
// compiling, a public header grew a dependency it must not have -- which is exactly the
// property the public interface relies on, and the reason BackendSession is opaque.

#include "ta_roienc/ffmpeg_log.h"
#include "ta_roienc/input_format.h"
#include "ta_roienc/packet.h"
#include "ta_roienc/ta_roienc.h"
#include "ta_roienc/timestamp.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <utility>

namespace {

// An embedder's own buffer, implemented with include/ and nothing else. This is the part
// of the surface that would be impossible if Frame carried a void* to an AVFrame: there
// would be nothing to implement, only a pointer to produce, and producing it would need
// FFmpeg. That it compiles here is the whole claim.
class CallerOwnedBuffer final : public ta_roienc::IFrameBuffer {
public:
    ta_roienc::MemoryType memory() const override { return ta_roienc::MemoryType::CPU; }
    int32_t width() const override { return 64; }
    int32_t height() const override { return 64; }
    ta_roienc::PixelFormat format() const override
    {
        return ta_roienc::PixelFormat::GRAY8;
    }
    ta_roienc::PlaneView plane(int32_t index) override
    {
        if (index != 0) {
            return {};
        }
        return ta_roienc::PlaneView{std::span<uint8_t>(m_pixels, sizeof(m_pixels)), 64};
    }

private:
    uint8_t m_pixels[64 * 64] = {};
};

// Naming the types is the test: each one has to be complete, and completing it drags in
// whatever its header includes.
[[maybe_unused]] void the_public_vocabulary_is_self_contained()
{
    ta_roienc::Settings settings;
    ta_roienc::SessionParams params;
    ta_roienc::Report report;
    ta_roienc::StreamInfo info;
    ta_roienc::EncodedPacket packet;
    ta_roienc::Rect rect;
    ta_roienc::Box box;
    ta_roienc::Mask mask;
    ta_roienc::Rational rational;

    // Input format, packet and timestamp. Named here rather than only in frame_test because
    // the claim being made is not that they work -- it is that they compile with no FFmpeg
    // anywhere. frame.h is deliberately absent: it is the one public header that
    // does name an AVFrame, and it is not reachable from ta_roienc.h.
    ta_roienc::InputVideoFormat input_format;
    ta_roienc::MediaTimestamp timestamp;
    ta_roienc::PacketBuffer payload;
    (void)input_format.valid();
    (void)timestamp.valid();
    (void)payload.empty();

    // Submitting a frame and receiving packets: Frame is a handle to an IFrameBuffer, which
    // an embedder implements with include/ and nothing else, and this compiles to prove it.
    auto owned = std::make_shared<CallerOwnedBuffer>();
    ta_roienc::Frame frame(owned);
    ta_roienc::TimedFrame timed{frame, ta_roienc::FrameMetadata{}};
    (void)static_cast<bool>(frame);
    (void)frame.buffer().width();
    (void)timed.metadata.force_key_frame;

    // The backend contract, opaque session and all.
    ta_roienc::BackendConfig config;
    ta_roienc::BackendFrameControl control;
    ta_roienc::EncoderCapabilities caps;
    const ta_roienc::Status status = ta_roienc::Status::Ok();
    const ta_roienc::BackendSelection selection;
    const std::vector<ta_roienc::BackendRef> backend =
        ta_roienc::resolve_backends(ta_roienc::Backend::X264, selection, ta_roienc::Codec::H264);

    // Fallback policy and the log sink: both are part of the public vocabulary, and
    // neither drags FFmpeg in with it.
    ta_roienc::Settings with_policy;
    with_policy.fallback = ta_roienc::FallbackPolicy::Strict;
    const ta_roienc::LogSink sink = [](ta_roienc::LogLevel, std::string_view) {};
    (void)ta_roienc::fallback_policy_name(with_policy.fallback);
    (void)ta_roienc::log_level_name(ta_roienc::LogLevel::Warning);
    (void)ta_roienc::log_level_name(ta_roienc::LogLevel::Trace);
    (void)sink;

    // The FFmpeg log routing. The header is about FFmpeg and deliberately names no FFmpeg
    // type, so that an embedder without FFmpeg on the include path can still compile
    // against the public surface. public_headers_selfcontained compiles it with
    // ${FFMPEG_INCLUDE_DIRS} on the path, which proves the opposite of what is wanted;
    // naming the function here is what puts it under this target.
    //
    // Declared, never called: this object library is never linked, and the definition lives
    // in the FFmpeg interop component that an isolated build does not have.
    const auto route = &ta_roienc::SetFFmpegLogSink;
    (void)route;
    (void)selection.allow_unlisted;

    // Memory types and objects.
    const ta_roienc::MemoryType memory = ta_roienc::MemoryType::ONEVPL;
    ta_roienc::Object object;

    CallerOwnedBuffer caller_buffer;
    caller_buffer.commit();

    // Planes the caller already has, without writing a class for them.
    uint8_t pixels[16 * 16] = {};
    const ta_roienc::PlaneBuffer::Plane described[4] = {
        {std::span<uint8_t>(pixels, sizeof(pixels)), 16}, {}, {}, {}};
    ta_roienc::PlaneBuffer plain(16, 16, ta_roienc::PixelFormat::GRAY8,
        std::span<const ta_roienc::PlaneBuffer::Plane>(described, 4));
    // A row past the end comes back empty rather than pointing somewhere.
    (void)plain.plane(0).row(999).size();
    (void)ta_roienc::pixel_format_name(ta_roienc::PixelFormat::NV12);

    // Wrapping planes the caller owns is an operation on plain memory, and it compiles in a
    // translation unit that has never seen libavutil.
    //
    // Nothing below runs: this target is an OBJECT library that is never linked, and
    // building it is passing it. What it proves is exactly one thing -- that the raw-plane
    // API can be spelled without FFmpeg on the include path. The owner contract and the
    // no-copy guarantee are behaviour, and frame_test is what checks them.
    auto owner = std::make_shared<std::vector<uint8_t>>(16 * 16, 0);
    ta_roienc::RawFrameView view;
    view.format.width = 16;
    view.format.height = 16;
    view.format.pixel_format = ta_roienc::PixelFormat::GRAY8;
    view.planes[0] = ta_roienc::RawFramePlane{owner->data(), 16, owner->size()};
    view.plane_count = 1;
    view.owner = owner;
    ta_roienc::Frame wrapped;
    const ta_roienc::Status wrap_status = ta_roienc::WrapRawFrame(view, &wrapped);
    (void)wrap_status; (void)wrapped;

    (void)settings; (void)params; (void)report; (void)info; (void)packet;
    (void)rect; (void)box; (void)mask; (void)rational; (void)config;
    (void)input_format; (void)timestamp; (void)payload;
    (void)control; (void)caps; (void)status; (void)backend; (void)memory;

    // The owning types are named, and the caller never writes a delete.
    ta_roienc::FrameBufferPtr owned_buffer;
    ta_roienc::SessionPtr owned_session;

    // And unique ownership is not a ceiling. A caller who needs a frame in a map, or alive
    // across threads, converts -- the deleter travels with it, so the library still frees
    // it with the allocator that made it. This compiles, which is the point: returning
    // unique_ptr from the factory leaves that choice open, and returning shared_ptr would
    // have charged every caller an atomic refcount for a choice most of them never make.
    std::shared_ptr<ta_roienc::IFrameBuffer> shared_buffer = std::move(owned_buffer);
    (void)shared_buffer; (void)owned_session;

    (void)object; (void)caller_buffer;
}

} // namespace
