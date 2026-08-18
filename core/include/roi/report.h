#pragma once

// What a completed encode has to say for itself. Shared by both entry points:
// the caller of either wants to know which backend actually ran, and with
// Encoder::Auto that is not knowable in advance.

#include "roi/types.h"

#include <cstdint>
#include <string>

namespace roi {

struct Report {
    Encoder encoder_used = Encoder::X265;
    std::string encoder_name;
    int64_t frames = 0;
    int64_t packets = 0;
    int64_t bytes = 0;
    // Regions the encoder's region budget could not take.
    int64_t regions_dropped = 0;
};

} // namespace roi
