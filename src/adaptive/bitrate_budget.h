#pragma once

// Outer bitrate controller for TZ v2.0 §15 -- a layer *above* the encoder, not a
// pass-through into its own rate control.
//
// Under CQP/CRF the per-region QP deltas stay honest only while the global quantizer is
// fixed. A target bitrate forces a second loop: watch the average bits out, and when the
// stream runs hot crush the background first, then soften low-importance ROI. High-
// importance regions (faces, plates) are left alone -- that is the object-first part.
//
// Peak overshoot (short window) and a recovery debt (give-back within recovery_window_ms)
// ride on the same ladder. This is still not encoder-side VBR/VBV.

#include "sve/policy.h"
#include "sve/types.h"

#include "object_normalizer.h"

#include <cstdint>
#include <vector>

namespace sve {

class BitrateBudget {
public:
    BitrateBudget() = default;
    explicit BitrateBudget(const RateControlPolicy& policy);

    bool active() const { return m_target_bps > 0; }

    /// Account for one encoded packet. `duration_sec` is how long it covers; when the
    /// packet carries no duration the caller should pass 1/fps.
    void observe(int64_t packet_bytes, double duration_sec);

    /// Extra (non-negative) added to Quality::background_qp_delta.
    int32_t background_extra() const { return m_background_extra; }

    /// Softening applied to non-critical ROI qp_deltas (moved toward / into a penalty).
    int32_t roi_soften() const { return m_roi_soften; }

    /// Outstanding bit debt the recovery window is trying to pay back. Test/diagnostics.
    double debt_bits() const { return m_debt_bits; }

    /// Copy of `base` with background_qp_delta raised by the current extra.
    Quality apply(const Quality& base) const;

    /// Soften low-importance detections in place. Regions at or above
    /// RateControlPolicy::critical_importance are untouched.
    void soften_detections(std::vector<Detection>& detections) const;

private:
    struct Chunk {
        double duration = 0.0;
        int64_t bytes = 0;
    };

    void recompute();
    void step_up();
    void step_down();
    void trim(std::vector<Chunk>& chunks, double& sec, int64_t& bytes, double limit);

    uint64_t m_target_bps = 0;
    float m_avg_overshoot = 0.1f;
    float m_peak_overshoot = 0.2f;
    double m_recovery_sec = 1.0;
    float m_critical_importance = 0.95f;

    std::vector<Chunk> m_avg_chunks;
    double m_avg_sec = 0.0;
    int64_t m_avg_bytes = 0;

    std::vector<Chunk> m_peak_chunks;
    double m_peak_sec = 0.0;
    int64_t m_peak_bytes = 0;

    double m_total_sec = 0.0;
    double m_last_avg_step_sec = 0.0;
    double m_last_peak_step_sec = 0.0;
    double m_debt_bits = 0.0;

    int32_t m_background_extra = 0;
    int32_t m_roi_soften = 0;
};

} // namespace sve
