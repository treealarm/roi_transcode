#pragma once

#include "bitrate_budget.h"
#include "box_smoother.h"
#include "object_normalizer.h"
#include "qp_slew.h"
#include "roi_lag.h"
#include "sve/capabilities.h"
#include "sve/policy.h"
#include "sve/types.h"

#include <span>
#include <vector>

namespace sve {

int32_t effective_coding_block(const EncoderCapabilities& caps);
int32_t effective_region_budget(const Quality& quality, const EncoderCapabilities& caps);

struct FrameRoiPipelineInput {
    std::span<const Box> boxes;
    const Quality& quality;
    const TemporalROIConfig& temporal;
    EncoderCapabilities caps;
    int32_t width = 0;
    int32_t height = 0;
    double frame_interval_sec = 0.0;
};

struct FrameRoiPipelineOutput {
    std::vector<Rect> rects;
    std::vector<Box> applied_boxes;
    std::vector<RoiLagSample> lag_samples;
    int32_t regions_dropped = 0;
    int32_t coding_block = 16;
};

class FrameRoiPipeline {
public:
    FrameRoiPipeline(BitrateBudget& bitrate, BoxSmoother& box_smooth,
        QpSlewLimiter& qp_slew, RoiMapBuilder& roi_builder)
        : m_bitrate(bitrate)
        , m_box_smooth(box_smooth)
        , m_qp_slew(qp_slew)
        , m_roi_builder(roi_builder)
    {
    }

    FrameRoiPipelineOutput process(const FrameRoiPipelineInput& in);

private:
    BitrateBudget& m_bitrate;
    BoxSmoother& m_box_smooth;
    QpSlewLimiter& m_qp_slew;
    RoiMapBuilder& m_roi_builder;
};

} // namespace sve
