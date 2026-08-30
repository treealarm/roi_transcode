#pragma once

// A timestamp that says what its own units are -- TZ v3.4 §8.7.
//
// The rule the type exists to enforce: nothing may assume milliseconds, microseconds or
// anything else. Every value travels with the time base it is counted in, and a conversion
// between two bases is written out rather than implied. Video code that guesses units is
// where audio drifts a second per hour and nobody can say which side did it.
//
// Used where a timestamp arrives apart from the pixels -- RawFrameView below, and the
// frame metadata the encoder takes. An EncodedPacket carries one time base for its pts,
// dts and duration together rather than three copies of the same rational.

#include "sve/types.h"

#include <cstdint>

namespace sve {

/// "No timestamp". FFmpeg spells this AV_NOPTS_VALUE and it is the same value, written
/// here so the public headers keep naming no libav* symbol.
inline constexpr int64_t kNoTimestamp = INT64_MIN;

struct MediaTimestamp {
    int64_t value = kNoTimestamp;
    Rational time_base{1, 90000};

    /// A timestamp with units. An unset value is not valid, and neither is a value whose
    /// time base cannot be divided by -- both are answers a caller has to handle, and
    /// neither is worth an exception.
    constexpr bool valid() const noexcept
    {
        return value != kNoTimestamp && time_base.num > 0 && time_base.den > 0;
    }
};

} // namespace sve
