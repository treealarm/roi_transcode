#include "ta_roienc/picture_view.h"

#include "owned_layer_store.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>

namespace ta_roienc {

namespace {

/// How many planes this layout has. Not derived from a pixel-format descriptor, because the
/// caller's layer_count is being checked against what the format means here, and the two
/// have to be the same short list the rest of the library works from.
std::size_t plane_count_of(PixelLayout f)
{
    switch (f) {
    case PixelLayout::YUV420P:
    case PixelLayout::YUV420P10:
    case PixelLayout::YUV422P:
    case PixelLayout::YUV444P:
        return 3;
    case PixelLayout::NV12:
    case PixelLayout::P010:
        return 2;
    case PixelLayout::GRAY8:
        return 1;
    case PixelLayout::Unknown:
    case PixelLayout::HardwareSurface:
        break;
    }
    return 0;
}

/// Rows this plane holds. Chroma is half height on 4:2:0 and full height elsewhere; NV12
/// interleaves both chroma planes into one, so its second plane is half height too.
int32_t plane_rows(PixelLayout f, std::size_t index, int32_t height)
{
    if (index == 0) {
        return height;
    }
    switch (f) {
    case PixelLayout::YUV420P:
    case PixelLayout::YUV420P10:
    case PixelLayout::NV12:
    case PixelLayout::P010:
        return (height + 1) / 2;
    default:
        return height;
    }
}

} // namespace

Verdict WrapPicture(const PictureView& view, Picture* output)
{
    if (output == nullptr) {
        return Verdict::InvalidArgument("WrapPicture needs somewhere to put the frame");
    }
    if (!view.format.valid()) {
        return Verdict::InvalidArgument("raw frame view carries no usable input format");
    }
    if (!view.owner) {
        // The whole reason the field exists. Wrapping without it produces a Picture that
        // outlives its pixels, and the failure lands inside an encoder several frames
        // later where nothing can attribute it.
        return Verdict::InvalidArgument(
            "raw frame view has no owner: pass a copy, or an owner that keeps the pixels alive");
    }

    const std::size_t expected = plane_count_of(view.format.pixel_layout);
    if (expected == 0) {
        return Verdict::Unsupported(std::format("cannot wrap pixel format {}",
            pixel_layout_name(view.format.pixel_layout)));
    }
    if (view.layer_count != expected) {
        return Verdict::InvalidArgument(std::format("pixel format {} has {} planes, {} described",
            pixel_layout_name(view.format.pixel_layout), expected, view.layer_count));
    }

    for (std::size_t i = 0; i < view.layer_count; ++i) {
        const PictureLayer& plane = view.planes[i];
        if (plane.data == nullptr || plane.stride <= 0) {
            return Verdict::InvalidArgument(std::format("plane {} has no pixels", i));
        }
        const int32_t rows = plane_rows(view.format.pixel_layout, i, view.format.height);
        const std::size_t needed =
            static_cast<std::size_t>(plane.stride) * static_cast<std::size_t>(rows);
        if (plane.size < needed) {
            return Verdict::InvalidArgument(std::format(
                "plane {} holds {} bytes, {} rows of stride {} need {}",
                i, plane.size, rows, plane.stride, needed));
        }
    }

    *output = Picture(std::make_shared<OwnedLayerStore>(view));
    return Verdict::Ok();
}

} // namespace ta_roienc
