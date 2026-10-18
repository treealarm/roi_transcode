#pragma once

#include "bit_allowance.h"
#include "box_steadier.h"
#include "object_cleanup.h"
#include "qp_rate_limit.h"
#include "roi_delay.h"
#include "ta_roienc/abilities.h"
#include "ta_roienc/rules.h"
#include "ta_roienc/basics.h"

#include <span>
#include <vector>

namespace ta_roienc {

int32_t effective_coding_block(const EngineAbilities& caps);
int32_t effective_region_budget(const QualityPlan& quality, const EngineAbilities& caps);

struct RoiPicturePipelineInput {
    std::span<const Box> boxes;
    const QualityPlan& quality;
    const RoiPersistence& temporal;
    EngineAbilities caps;
    int32_t width = 0;
    int32_t height = 0;
    double frame_interval_sec = 0.0;
};

struct RoiPicturePipelineOutput {
    std::vector<Area> rects;
    std::vector<Box> applied_boxes;
    std::vector<RoiDelaySample> lag_samples;
    int32_t regions_dropped = 0;
    int32_t coding_block = 16;
};

class RoiPicturePipeline {
public:
    RoiPicturePipeline(BitAllowance& bitrate, BoxSmoother& box_smooth,
        QpRateLimiter& qp_slew, RoiGridBuilder& roi_builder)
        : m_bitrate(bitrate)
        , m_box_smooth(box_smooth)
        , m_qp_slew(qp_slew)
        , m_roi_builder(roi_builder)
    {
    }

    RoiPicturePipelineOutput process(const RoiPicturePipelineInput& in);

private:
    BitAllowance& m_bitrate;
    BoxSmoother& m_box_smooth;
    QpRateLimiter& m_qp_slew;
    RoiGridBuilder& m_roi_builder;
};

} // namespace ta_roienc
