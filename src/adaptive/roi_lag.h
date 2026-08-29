#pragma once

// ROI lag helpers -- TZ v3 §14.
//
// Acceptance talks about IoU between the applied ROI and the target box, and about how many
// frames / milliseconds the applied region trails the target. Pure geometry: no codec.

#include "smartvideo/types.h"
#include "smartvideo/report.h"

#include <cstdint>
#include <vector>

namespace smartvideo {

/// Intersection-over-union of two axis-aligned boxes in normalized coordinates. Empty or
/// non-overlapping boxes yield 0.
float box_iou(const Box& a, const Box& b);

struct RoiLagSample {
    /// IoU between the region that was applied and the ground-truth target at the same t.
    float iou = 0.0f;
    /// How many producer frames the applied box is behind the target (0 = same sample).
    int32_t lag_frames = 0;
    /// Same lag in milliseconds, given the producer's frame interval.
    double lag_ms = 0.0;
};

/// Compare `applied` (what TemporalRoi / the encoder used) to `target` (detector truth at
/// the same t). `producer_interval_sec` turns a frame count into milliseconds.
RoiLagSample measure_roi_lag(const Box& applied, const Box& target, int32_t lag_frames,
    double producer_interval_sec);

/// P95 / P99 of a sorted-or-unsorted list of values. Empty → 0.
double percentile(std::vector<double> values, double p);

/// Whether a set of lag samples meets v3 §14 acceptance:
/// IoU ≥ 0.50 on every sample that claims a match; P95 lag ≤ 3 frames and ≤ 100 ms;
/// P99 ≤ 6 frames and ≤ 200 ms.
bool roi_lag_acceptance(const std::vector<RoiLagSample>& samples);

/// Normalized center distance in frame-diagonal units.
float center_distance_diag(const Box& a, const Box& b);

/// Fills §14 lag observability fields in Report from runtime samples.
void fill_lag_report(const std::vector<RoiLagSample>& samples, Report& report);

} // namespace smartvideo
