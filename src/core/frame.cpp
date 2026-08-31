#include "sve/frame.h"

#include "backend/common/av_frame_buffer.h"
#include "pixel_format.h"

#include <cstdint>
#include <format>
#include <memory>
#include <new>

namespace sve {

namespace {

/// An owned AVFrame behind the backend-neutral interface. AvFrameBuffer already knows how
/// to own one -- CreateFrame relies on it -- so the adapter is a construction, not a new
/// implementation.
std::shared_ptr<IFrameBuffer> own_av_frame(AVFrame* frame)
{
    auto buffer = std::make_shared<AvFrameBuffer>(nullptr);
    buffer->adopt(frame, nullptr, MemoryType::AVFRAME);
    return buffer;
}

} // namespace

Frame AvFrameAdapter::Ref(const AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr) {
        return {};
    }
    if (av_frame_ref(copy, frame) < 0) {
        av_frame_free(&copy);
        return {};
    }
    return Frame(own_av_frame(copy));
}

Frame AvFrameAdapter::Adopt(AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    return Frame(own_av_frame(frame));
}

Status SubmitAvFrame(SmartVideoEncoder& encoder, const AVFrame* frame,
    const FrameMetadata& metadata)
{
    if (frame == nullptr) {
        return Status::InvalidArgument("SubmitAvFrame needs a frame");
    }
    Frame held = AvFrameAdapter::Ref(frame);
    if (!held) {
        return Status::OutOfMemory("could not reference the frame");
    }
    return encoder.SubmitFrame(TimedFrame{std::move(held), metadata});
}

namespace {

/// How many planes this layout has. Not derived from the AVPixFmtDescriptor, because the
/// caller's plane_count is being checked against what the format means here, and the two
/// have to be the same short list the rest of the library works from.
std::size_t plane_count_of(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV444P:
        return 3;
    case PixelFormat::NV12:
    case PixelFormat::P010:
        return 2;
    case PixelFormat::GRAY8:
        return 1;
    case PixelFormat::Unknown:
    case PixelFormat::HardwareSurface:
        break;
    }
    return 0;
}

/// Rows this plane holds. Chroma is half height on 4:2:0 and full height elsewhere; NV12
/// interleaves both chroma planes into one, so its second plane is half height too.
int32_t plane_rows(PixelFormat f, std::size_t index, int32_t height)
{
    if (index == 0) {
        return height;
    }
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::NV12:
    case PixelFormat::P010:
        return (height + 1) / 2;
    default:
        return height;
    }
}

} // namespace

Status WrapRawFrame(const RawFrameView& view, Frame* output)
{
    if (output == nullptr) {
        return Status::InvalidArgument("WrapRawFrame needs somewhere to put the frame");
    }
    if (!view.format.valid()) {
        return Status::InvalidArgument("raw frame view carries no usable input format");
    }
    if (!view.owner) {
        // The whole reason the field exists. Wrapping without it produces a Frame that
        // outlives its pixels, and the failure lands inside an encoder several frames
        // later where nothing can attribute it.
        return Status::InvalidArgument(
            "raw frame view has no owner: pass a copy, or an owner that keeps the pixels alive");
    }

    const std::size_t expected = plane_count_of(view.format.pixel_format);
    if (expected == 0) {
        return Status::Unsupported(std::format("cannot wrap pixel format {}",
            pixel_format_name(view.format.pixel_format)));
    }
    if (view.plane_count != expected) {
        return Status::InvalidArgument(std::format("pixel format {} has {} planes, {} described",
            pixel_format_name(view.format.pixel_format), expected, view.plane_count));
    }

    const int av_format = to_av_pix_fmt(view.format.pixel_format);
    if (av_format < 0) {
        return Status::Unsupported(std::format("no FFmpeg equivalent for pixel format {}",
            pixel_format_name(view.format.pixel_format)));
    }

    for (std::size_t i = 0; i < view.plane_count; ++i) {
        const RawFramePlane& plane = view.planes[i];
        if (plane.data == nullptr || plane.stride <= 0) {
            return Status::InvalidArgument(std::format("plane {} has no pixels", i));
        }
        const int32_t rows = plane_rows(view.format.pixel_format, i, view.format.height);
        const std::size_t needed =
            static_cast<std::size_t>(plane.stride) * static_cast<std::size_t>(rows);
        if (plane.size < needed) {
            return Status::InvalidArgument(std::format(
                "plane {} holds {} bytes, {} rows of stride {} need {}",
                i, plane.size, rows, plane.stride, needed));
        }
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        return Status::Error("out of memory allocating a frame");
    }

    // The owner rides along inside the frame's own buffer list, so releasing the frame
    // releases the caller's pixels and nothing else has to remember to. Documenting the
    // lifetime and not holding it would leave the one rule this function exists to enforce
    // resting on the caller doing it right.
    //
    // The raw new/delete pair is the one place in this library that has them, and it is
    // not an oversight: av_buffer_create takes a void* opaque and a C function pointer, so
    // there is nothing for a smart pointer to be handed across. The pair is three lines
    // apart and both ends are visible here; av_buffer_create failing is the only path
    // between them, and it deletes.
    auto* held = new std::shared_ptr<const void>(view.owner);
    AVBufferRef* keepalive = av_buffer_create(nullptr, 0,
        [](void* opaque, uint8_t*) { delete static_cast<std::shared_ptr<const void>*>(opaque); },
        held, AV_BUFFER_FLAG_READONLY);
    if (keepalive == nullptr) {
        delete held;
        av_frame_free(&frame);
        return Status::Error("out of memory holding the frame's owner");
    }
    frame->buf[0] = keepalive;

    frame->format = av_format;
    frame->width = view.format.width;
    frame->height = view.format.height;
    frame->pts = view.timestamp.valid() ? view.timestamp.value : kNoTimestamp;
    for (std::size_t i = 0; i < view.plane_count; ++i) {
        // const_cast because AVFrame::data is non-const and the encoder only reads it.
        // The bytes stay the caller's; nothing here writes through these pointers.
        frame->data[i] = const_cast<uint8_t*>(view.planes[i].data);
        frame->linesize[i] = view.planes[i].stride;
    }

    *output = AvFrameAdapter::Adopt(frame);
    return Status::Ok();
}

} // namespace sve
