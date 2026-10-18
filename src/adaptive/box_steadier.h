#pragma once

// EMA on box geometry across frames (spatial_smoothing / temporal_smoothing).
// Stops a detector's jitter from painting a flickering ROI even when QP is already slewed.

#include "ta_roienc/rules.h"
#include "ta_roienc/basics.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ta_roienc {

class BoxSmoother {
public:
    /// In-place. `temporal` is the weight on the new x/y; `spatial` on the new width/height.
    /// Either 0 leaves that part untouched. Untracked boxes (track_id == 0) are left alone:
    /// there is no identity to smooth across frames.
    void apply(std::vector<Box>& boxes, float temporal, float spatial);

    /// Nothing when temporal ROI is switched off: smoothing is one of the behaviours
    /// RoiPersistence::enabled governs, and half of a switched-off feature is a fourth
    /// behaviour nobody asked for.
    void apply(std::vector<Box>& boxes, const RoiPersistence& cfg)
    {
        if (!cfg.enabled) {
            return;
        }
        apply(boxes, cfg.temporal_smoothing, cfg.spatial_smoothing);
    }

private:
    struct Phase {
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
    };

    static uint64_t key_of(const Box& b);

    std::unordered_map<uint64_t, Phase> m_state;
};

} // namespace ta_roienc
