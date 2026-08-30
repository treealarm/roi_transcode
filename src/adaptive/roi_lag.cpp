#include "roi_lag.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sve {

float box_iou(const Box& a, const Box& b)
{
    const float ax2 = a.x + a.width;
    const float ay2 = a.y + a.height;
    const float bx2 = b.x + b.width;
    const float by2 = b.y + b.height;
    const float ix1 = std::max(a.x, b.x);
    const float iy1 = std::max(a.y, b.y);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) {
        return 0.0f;
    }
    const float inter = iw * ih;
    const float uni = a.width * a.height + b.width * b.height - inter;
    if (uni <= 0.0f) {
        return 0.0f;
    }
    return inter / uni;
}

RoiLagSample measure_roi_lag(const Box& applied, const Box& target, int32_t lag_frames,
    double producer_interval_sec)
{
    RoiLagSample s;
    s.iou = box_iou(applied, target);
    s.lag_frames = std::max(lag_frames, 0);
    s.lag_ms = static_cast<double>(s.lag_frames) * std::max(producer_interval_sec, 0.0) * 1000.0;
    return s;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped = std::clamp(p, 0.0, 1.0);
    const double idx = clamped * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) {
        return values[lo];
    }
    const double t = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

bool roi_lag_acceptance(const std::vector<RoiLagSample>& samples)
{
    if (samples.empty()) {
        return false;
    }
    std::vector<double> frames;
    std::vector<double> ms;
    frames.reserve(samples.size());
    ms.reserve(samples.size());
    for (const RoiLagSample& s : samples) {
        if (s.iou < 0.50f) {
            return false;
        }
        frames.push_back(static_cast<double>(s.lag_frames));
        ms.push_back(s.lag_ms);
    }
    return percentile(frames, 0.95) <= 3.0 && percentile(ms, 0.95) <= 100.0
        && percentile(frames, 0.99) <= 6.0 && percentile(ms, 0.99) <= 200.0;
}

float center_distance_diag(const Box& a, const Box& b)
{
    const float ax = a.x + 0.5f * a.width;
    const float ay = a.y + 0.5f * a.height;
    const float bx = b.x + 0.5f * b.width;
    const float by = b.y + 0.5f * b.height;
    const float dx = bx - ax;
    const float dy = by - ay;
    constexpr float diagonal = 1.41421356f; // sqrt(2) in normalized space
    return static_cast<float>(std::sqrt(dx * dx + dy * dy) / diagonal);
}

void fill_lag_report(const std::vector<RoiLagSample>& samples, Report& report)
{
    report.roi_lag_measured = !samples.empty();
    report.roi_lag_samples = static_cast<int64_t>(samples.size());
    if (samples.empty()) {
        return;
    }

    std::vector<double> frames;
    std::vector<double> ms;
    frames.reserve(samples.size());
    ms.reserve(samples.size());
    float min_iou = std::numeric_limits<float>::max();
    for (const RoiLagSample& s : samples) {
        min_iou = std::min(min_iou, s.iou);
        frames.push_back(static_cast<double>(s.lag_frames));
        ms.push_back(s.lag_ms);
    }
    report.roi_lag_min_iou = min_iou;
    report.roi_lag_p95_frames = percentile(frames, 0.95);
    report.roi_lag_p99_frames = percentile(frames, 0.99);
    report.roi_lag_p95_ms = percentile(ms, 0.95);
    report.roi_lag_p99_ms = percentile(ms, 0.99);
    report.roi_lag_acceptance = roi_lag_acceptance(samples);
}

} // namespace sve
