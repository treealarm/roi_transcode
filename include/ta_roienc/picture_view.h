#pragma once

// Raw planes API.
//
// Pixels a caller already has, wrapped into a Picture without copying. Nothing about "here
// are my planes" needs libavutil, so this lives outside ta_roienc/picture.h and the implementation
// does not go through AVFrame.
//
// ta_roienc/picture.h includes this header for convenience.

#include "ta_roienc/picture_store.h"
#include "ta_roienc/source_format.h"
#include "ta_roienc/verdict.h"
#include "ta_roienc/stamped_picture.h"
#include "ta_roienc/stamp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ta_roienc {

/// One plane of pixels the caller owns, with the length that bounds it. A pointer alone
/// carries no length, and a stride passed separately from the pointer it belongs to is the
/// oldest bug in video code.
struct PictureLayer {
    const uint8_t* data = nullptr;
    int32_t stride = 0;
    std::size_t size = 0;
};

/// Pixels the caller has somewhere else, described well enough to wrap without copying.
///
/// `owner` is the part that makes this safe and the part a caller most wants to skip. It
/// keeps the pixels alive for as long as the resulting Picture exists -- WrapPicture holds
/// it inside the frame, so this is enforced rather than merely asked for. A view whose
/// owner is empty is refused rather than wrapped: the alternative is a Picture pointing into
/// a buffer that was freed while the encoder still held it. A caller who cannot make that
/// promise passes a copy instead.
struct PictureView {
    SourceFormat format;
    std::array<PictureLayer, 4> planes{};
    std::size_t layer_count = 0;
    /// Carried for the caller's convenience, not read by the library: the timestamp an
    /// encoder uses comes from PictureInfo, which is what SubmitPicture takes beside the
    /// frame. A view whose timestamp disagrees with the metadata loses that argument.
    Stamp timestamp;
    std::shared_ptr<const void> owner;
};

/// Wraps `view` into `output` without copying pixels. Validates the format, the plane
/// count for that format, and that every plane holds the rows its stride and the frame
/// height claim -- so a short buffer is an InvalidArgument here rather than a read past
/// the end inside an encoder.
Verdict WrapPicture(const PictureView& view, Picture* output);

} // namespace ta_roienc
