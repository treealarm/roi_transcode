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
#include <string>
#include <utility>

namespace sve {

/// A refusal under FallbackPolicy::Strict -- §29's "unsupported feature" event.
///
/// The Status on its own does not make one: it reaches the caller and nobody else, so a
/// refusal that stopped an encode was invisible to whoever reads the log afterwards to
/// find out why. One text serves both, so the log and the Status can never describe
/// different refusals.
///
/// Announced once, on the same flag the permissive path uses. A guard consulted per frame
/// would otherwise say it sixty times a second, which is the noise the rest of this file is
/// written to avoid; `warned` is set here so that the two paths cannot both announce.
inline Status refuse_unsupported(const Logger& log, std::string message, bool& warned)
{
    if (!warned) {
        log.error("unsupported: " + message);
        warned = true;
    }
    return Status::Unsupported(std::move(message));
}

/// Bitstream metadata was asked for and the encoder cannot carry it (§19.2).
inline Status guard_sei_unsupported(const bool requested, const EncoderCapabilities& caps,
    const FallbackPolicy policy, const Logger& log, bool& warned)
{
    if (!requested || caps.sei) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        return refuse_unsupported(log,
            "bitstream SEI requested but not supported by encoder capabilities", warned);
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
        return refuse_unsupported(log,
            std::format("{} roi region(s) do not fit the {} backend limit of {}", dropped,
                codec == Codec::H264 ? "h264" : "h265", caps.max_roi_regions),
            warned);
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
        return refuse_unsupported(log,
            "a per-block quality map was supplied and this backend takes rectangles only",
            warned);
    }
    if (!warned) {
        log.warn("guard: per-block quality map quantized to rectangles for this backend");
        warned = true;
    }
    return Status::Ok();
}

/// The per-block quality map wanted a quantiser the codec does not have (§50).
///
/// Not a capability the backend lacks but a scale that ran out: a delta is added to the
/// frame's own quantiser, and the sum can fall outside 1..51 whatever the backend. The
/// caller asked for a degradation and got a smaller one, which is exactly the kind of
/// quiet shortfall §50 exists to forbid -- measured on the native oneVPL path at crf 28,
/// where background deltas of 30 and of 40 produce byte-identical output because both
/// clamp to 51.
///
/// Permissive only. There is no Strict form because there is nothing to refuse: the map
/// was applied, as far as the codec's scale reaches, and no other behaviour is available.
inline void guard_quantiser_clamped(
    const int64_t frames, const Logger& log, bool& warned)
{
    if (frames <= 0 || warned) {
        return;
    }
    log.warn(std::format(
        "guard: the quality map ran out of quantiser scale on {} frame(s); the request "
        "beyond it did nothing", frames));
    warned = true;
}

/// A target bitrate was asked for on CPU H.265, where the path is unstable (STATUS.md Р5).
inline Status guard_cpu_h265_target(const bool requested, const FallbackPolicy policy,
    const Logger& log)
{
    if (!requested) {
        return Status::Ok();
    }
    if (policy == FallbackPolicy::Strict) {
        // Announced every time, unlike the per-frame guards above, and deliberately.
        // Those fire again without the caller having done anything; this one is reached
        // only from open and from Reconfigure, which is the caller asking again. Answering
        // each ask is not repetition.
        bool once = false;
        return refuse_unsupported(log,
            "cpu h265 has no target-bitrate mode in this build; see STATUS.md Р5", once);
    }
    log.warn("guard: cpu h265 target-bitrate disabled; using CRF/CQP");
    return Status::Ok();
}

} // namespace sve
