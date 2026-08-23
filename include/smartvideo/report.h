#pragma once

// What a completed encode has to say for itself. Shared by both entry points:
// the caller of either wants to know which backend actually ran, and with
// Encoder::Auto that is not knowable in advance.

#include "smartvideo/types.h"

#include <cstdint>
#include <string>

namespace smartvideo {

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
};

} // namespace smartvideo
