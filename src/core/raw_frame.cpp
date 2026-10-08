#include "ta_roienc/raw_frame.h"

#include "owned_plane_buffer.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>

namespace ta_roienc {

namespace {

/// How many planes this layout has. Not derived from a pixel-format descriptor, because the
/// caller's plane_count is being checked against what the format means here, and the two
/// have to be the same short list the rest of the library works from.
std::size_t plane_count_of(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
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
    case PixelFormat::YUV420P10:
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

    *output = Frame(std::make_shared<OwnedPlaneBuffer>(view));
    return Status::Ok();
}

} // namespace ta_roienc
