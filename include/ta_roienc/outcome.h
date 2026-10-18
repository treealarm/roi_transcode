#pragma once

// What a completed encode has to say for itself. Shared by both entry points:
// the caller of either wants to know which backend actually ran, and with
// Engine::Auto that is not knowable in advance.

#include "ta_roienc/basics.h"

#include <cstdint>
#include <string>

namespace ta_roienc {

struct Outcome {
    Engine engine_used = Engine::X264;
    std::string engine_name;
    /// Where the sidecar was written, empty when none was asked for. The library writes
    /// records and the application names the file, so this is the application saying which
    /// name it chose rather than the encoder reporting its own doing.
    std::string metadata_file_path;
    int64_t frames = 0;
    int64_t packets = 0;
    int64_t bytes = 0;
    // Regions the encoder's region budget could not take.
    int64_t regions_dropped = 0;

    // How many frames had to be converted to the encoder's pixel format, and how many had
    // to be uploaded to the GPU, before the encoder would take them.
    //
    // Reported rather than assumed. "Zero-copy" is otherwise a claim nobody can check: a
    // frame from CreatePicture that the caller filled in place leaves both at zero, and a
    // frame that merely looked like one leaves a count behind. Both being non-zero is not
    // a fault -- it is what submitting ordinary planes to a hardware encoder costs.
    int64_t frames_converted = 0;
    int64_t frames_uploaded = 0;

    // Which decoder ran, for a session that was given compressed input rather than raw
    // frames. Empty for a raw session, which decodes nothing.
    std::string decoder_name;
    // The hardware the decoded frames came back on ("vaapi", "qsv"), empty when the decode
    // ran on the CPU.
    //
    // Reported for the same reason frames_uploaded is. A session whose encoder is on the GPU
    // and whose decoder is not still moves every frame across the bus twice, and the two
    // cases are indistinguishable from the outside -- both work, one costs several times the
    // CPU. Non-empty here together with frames_uploaded at zero is what an end-to-end
    // hardware path looks like.
    std::string decoder_hardware;

    // Safety guard for this build: CPU H.265 ignores target_bitrate and runs plain
    // CRF/CQP to avoid an unstable ROI+SEI path in libx265.
    bool cpu_h265_target_guard = false;

    // ROI lag observability. Filled when tracked boxes are available on the path.
    bool roi_lag_measured = false;
    int64_t roi_lag_samples = 0;
    float roi_lag_min_iou = 0.0f;
    double roi_lag_p95_frames = 0.0;
    double roi_lag_p99_frames = 0.0;
    double roi_lag_p95_ms = 0.0;
    double roi_lag_p99_ms = 0.0;
    bool roi_lag_acceptance = false;
};

} // namespace ta_roienc
