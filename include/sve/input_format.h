#pragma once

// What the encoder is being fed, apart from what it is being asked to produce --
// TZ v3.4 §8.3.
//
// The split is the point. Frame geometry, pixel layout and the two rationals belong to the
// input; bitrate, GOP, preset and rate control belong to the output. Holding them in one
// structure is how a caller ends up changing a resolution while meaning to change a
// bitrate, and how "what does time_base apply to" stops having one answer.
//
// The format is fixed before the first frame and cannot change afterwards: an encoder that
// re-negotiated its input mid-stream would have to re-open the backend, which is a new
// encode with a new codec configuration, not a reconfiguration of this one.
//
// Departure from the letter of §8.3, named rather than discovered: width and height are
// int32_t, not uint32_t. Every dimension in this library is signed -- geometry, strides,
// block grids -- and one unsigned pair in the middle of that is where a subtraction wraps
// to four billion.

#include "sve/frame_buffer.h"
#include "sve/types.h"

#include <cstdint>

namespace sve {

struct InputVideoFormat {
    PixelFormat pixel_format = PixelFormat::YUV420P;
    int32_t width = 0;
    int32_t height = 0;
    /// Only sets the encoder's frame rate and the default GOP; it does not resample.
    Rational frame_rate{30, 1};
    /// Units of every timestamp on the way in and on the way out.
    Rational time_base{1, 90000};

    /// Whether this describes a frame that could exist. Checked at Create rather than at
    /// the first submit, so a caller learns about a zero height before they have encoded
    /// anything.
    constexpr bool valid() const noexcept
    {
        return width > 0 && height > 0 && pixel_format != PixelFormat::Unknown
            && frame_rate.num > 0 && frame_rate.den > 0
            && time_base.num > 0 && time_base.den > 0;
    }
};

} // namespace sve
