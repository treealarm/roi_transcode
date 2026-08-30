#pragma once

// Limit how fast a region's QP may move between frames -- TZ v3 §13 max_qp_delta_per_frame.
// Without this a track that flips priority (or a detector that blinks) paints a visible
// flicker; the ceiling turns that step into a ramp.

#include "object_normalizer.h"

#include <cstdint>
#include <unordered_map>

namespace sve {

class QpSlewLimiter {
public:
    /// `max_step` 0 disables. Positive: each detection's qp_delta may move by at most this
    /// many units from the last frame with the same track_id (or geometry key if untracked).
    void apply(std::vector<Detection>& detections, int32_t max_step);

private:
    static uint64_t key_of(const Detection& d);

    std::unordered_map<uint64_t, int32_t> m_last_qp;
};

} // namespace sve
