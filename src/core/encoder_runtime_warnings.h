#pragma once

// The degradations this library is allowed to apply, and the noise it must make when it
// does -- TZ v3.4 §22 and §50.
//
// Each one is a place where the hardware cannot do what was asked and there is a lesser
// thing that can be done instead. Under FallbackPolicy::Permissive the lesser thing
// happens and is announced once; under Strict nothing happens and the caller is told.
//
// "Announced once" rather than per frame: a warning repeated sixty times a second is a
// warning nobody reads.

#include "logger.h"
#include "sve/capabilities.h"
#include "sve/policy.h"
#include "sve/status.h"
#include "sve/types.h"

#include <format>

namespace sve {

/// Bitstream metadata was asked for and the encoder cannot carry it (§19.2).
inline Status guard_sei_unsupported(const bool requested, const EncoderCapabilities& caps,
    const FallbackPolicy policy, const Logger& log, bool& warned)
{
    if (!requested || caps.sei) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        return Status::Unsupported(
            "bitstream SEI requested but not supported by encoder capabilities");
    }
    if (!warned) {
        log.warn("warning: bitstream SEI requested but not supported by encoder capabilities");
        warned = true;
    }
    return Status::Ok();
}

/// Regions were cut down to what the backend accepts (§22, first two rows).
inline Status guard_roi_regions_dropped(const int64_t dropped, const EncoderCapabilities& caps,
    const Codec codec, const FallbackPolicy policy, const Logger& log, bool& warned)
{
    if (dropped <= 0) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        return Status::Unsupported(std::format(
            "{} roi region(s) do not fit the {} backend limit of {}",
            dropped, codec == Codec::H264 ? "h264" : "h265", caps.max_roi_regions));
    }
    if (!warned) {
        log.warn(std::format("guard: {} roi region(s) dropped; {} backend takes {}",
            dropped, codec == Codec::H264 ? "h264" : "h265", caps.max_roi_regions));
        warned = true;
    }
    return Status::Ok();
}

/// A block-level quality map was supplied and the backend takes rectangles only. §22 calls
/// this "quantize to ROI"; it is what happens to a mask on every backend here.
inline Status guard_qp_map_quantized(const bool have_map, const EncoderCapabilities& caps,
    const FallbackPolicy policy, const Logger& log, bool& warned)
{
    if (!have_map || caps.qp_map) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        return Status::Unsupported(
            "a per-block quality map was supplied and this backend takes rectangles only");
    }
    if (!warned) {
        log.warn("guard: per-block quality map quantized to rectangles for this backend");
        warned = true;
    }
    return Status::Ok();
}

/// A target bitrate was asked for on CPU H.265, where the path is unstable (STATUS.md Р5).
inline Status guard_cpu_h265_target(const bool requested, const FallbackPolicy policy,
    const Logger& log)
{
    if (!requested) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        return Status::Unsupported(
            "cpu h265 has no target-bitrate mode in this build; see STATUS.md Р5");
    }
    log.warn("guard: cpu h265 target-bitrate disabled; using CRF/CQP");
    return Status::Ok();
}

} // namespace sve
