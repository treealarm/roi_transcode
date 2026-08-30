#pragma once

// EMA on box geometry across frames -- TZ v3 §13 spatial_smoothing / temporal_smoothing.
// Stops a detector's jitter from painting a flickering ROI even when QP is already slewed.

#include "sve/policy.h"
#include "sve/types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sve {

class BoxSmoother {
public:
    /// In-place. `temporal` is the weight on the new x/y; `spatial` on the new width/height.
    /// Either 0 leaves that part untouched. Untracked boxes (track_id == 0) are left alone:
    /// there is no identity to smooth across frames.
    void apply(std::vector<Box>& boxes, float temporal, float spatial);

    void apply(std::vector<Box>& boxes, const TemporalROIConfig& cfg)
    {
        apply(boxes, cfg.temporal_smoothing, cfg.spatial_smoothing);
    }

private:
    struct State {
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
    };

    static uint64_t key_of(const Box& b);

    std::unordered_map<uint64_t, State> m_state;
};

} // namespace sve
