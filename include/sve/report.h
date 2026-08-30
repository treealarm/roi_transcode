#pragma once

// What a completed encode has to say for itself. Shared by both entry points:
// the caller of either wants to know which backend actually ran, and with
// Encoder::Auto that is not knowable in advance.

#include "sve/types.h"

#include <cstdint>
#include <string>

namespace sve {

struct Report {
    Encoder encoder_used = Encoder::Cpu;
    std::string encoder_name;
    int64_t frames = 0;
    int64_t packets = 0;
    int64_t bytes = 0;
    // Regions the encoder's region budget could not take.
    int64_t regions_dropped = 0;

    // How many frames had to be converted to the encoder's pixel format, and how many had
    // to be uploaded to the GPU, before the encoder would take them.
    //
    // Reported rather than assumed. "Zero-copy" is otherwise a claim nobody can check: a
    // frame from CreateFrame that the caller filled in place leaves both at zero, and a
    // frame that merely looked like one leaves a count behind. Both being non-zero is not
    // a fault -- it is what submitting ordinary planes to a hardware encoder costs.
    int64_t frames_converted = 0;
    int64_t frames_uploaded = 0;

    // Safety guard for this build: CPU H.265 ignores target_bitrate and runs plain
    // CRF/CQP to avoid an unstable ROI+SEI path in libx265.
    bool cpu_h265_target_guard = false;

    // §14 ROI lag observability. Filled when tracked boxes are available on the path.
    bool roi_lag_measured = false;
    int64_t roi_lag_samples = 0;
    float roi_lag_min_iou = 0.0f;
    double roi_lag_p95_frames = 0.0;
    double roi_lag_p99_frames = 0.0;
    double roi_lag_p95_ms = 0.0;
    double roi_lag_p99_ms = 0.0;
    bool roi_lag_acceptance = false;
};

} // namespace sve
