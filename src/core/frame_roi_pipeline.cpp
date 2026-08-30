#include "frame_roi_pipeline.h"

#include <algorithm>
#include <cmath>

namespace sve {

int32_t effective_coding_block(const EncoderCapabilities& caps)
{
    const int32_t b = qp_map_block(caps);
    return b > 0 ? b : 16;
}

int32_t effective_region_budget(const Quality& quality, const EncoderCapabilities& caps)
{
    if (caps.qp_map) {
        return 0;
    }
    int32_t budget = quality.max_regions;
    const int32_t backend_cap = static_cast<int32_t>(caps.max_roi_regions);
    if (backend_cap > 0) {
        budget = budget > 0 ? std::min(budget, backend_cap) : backend_cap;
    }
    return budget;
}

FrameRoiPipelineOutput FrameRoiPipeline::process(const FrameRoiPipelineInput& in)
{
    FrameRoiPipelineOutput out;
    out.coding_block = effective_coding_block(in.caps);
    const int32_t budget = effective_region_budget(in.quality, in.caps);

    std::vector<Box> target(in.boxes.begin(), in.boxes.end());
    out.applied_boxes = target;
    m_box_smooth.apply(out.applied_boxes, in.temporal);

    for (const Box& applied : out.applied_boxes) {
        if (applied.track_id == 0) {
            continue;
        }
        const auto it = std::find_if(target.begin(), target.end(),
            [&](const Box& b) {
                return b.track_id == applied.track_id && b.kind == applied.kind;
            });
        if (it == target.end()) {
            continue;
        }
        int32_t lag_frames = 0;
        const float speed = std::max(it->track.speed, 0.0f);
        if (speed > 1e-6f && in.frame_interval_sec > 0.0) {
            const double per_frame = static_cast<double>(speed) * in.frame_interval_sec;
            const double dist = static_cast<double>(center_distance_diag(applied, *it));
            lag_frames = static_cast<int32_t>(std::lround(dist / per_frame));
        }
        out.lag_samples.push_back(
            measure_roi_lag(applied, *it, lag_frames, in.frame_interval_sec));
    }

    auto dets = m_roi_builder.detections_from(
        out.applied_boxes, in.quality, in.width, in.height);
    m_bitrate.soften_detections(dets);
    // Under target bitrate, the outer loop must be free to move QP aggressively.
    const int32_t slew = m_bitrate.active() ? 0 : in.temporal.max_qp_delta_per_frame;
    m_qp_slew.apply(dets, slew);
    const Quality effective_quality = m_bitrate.apply(in.quality);
    out.rects =
        m_roi_builder.build(in.width, in.height, dets, effective_quality, out.coding_block, budget);
    if (budget > 0 && static_cast<int32_t>(out.rects.size()) > budget) {
        truncate_roi_rects(out.rects, budget, in.width, in.height, &out.regions_dropped);
    }
    return out;
}

} // namespace sve
