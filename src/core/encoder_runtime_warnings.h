#pragma once

#include "sve/capabilities.h"
#include "sve/types.h"

#include <cstdio>

namespace sve {

inline void warn_sei_unsupported_once(const bool requested, const EncoderCapabilities& caps,
    bool& warned)
{
    if (!requested || caps.sei || warned) {
        return;
    }
    std::fprintf(stderr,
        "warning: bitstream SEI requested but not supported by encoder capabilities\n");
    warned = true;
}

/// Says out loud that regions were cut to what the backend takes.
///
/// The count was already kept in Report::regions_dropped and went nowhere else, so on the
/// CLI paths a ROI set larger than the hardware's limit was quietly encoded as a smaller
/// one. TZ v3.4 §50 forbids exactly that -- a degradation has to be observable -- and this
/// is the same shape as the guard above: once per run, on stderr, naming the number.
inline void warn_roi_regions_dropped_once(
    const int64_t dropped, const EncoderCapabilities& caps, const Codec codec, bool& warned)
{
    if (dropped <= 0 || warned) {
        return;
    }
    std::fprintf(stderr,
        "guard: %lld roi region(s) dropped; %s backend takes %u\n",
        static_cast<long long>(dropped), codec == Codec::H264 ? "h264" : "h265",
        caps.max_roi_regions);
    warned = true;
}

} // namespace sve
