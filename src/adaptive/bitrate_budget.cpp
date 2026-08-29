// Outer bitrate controller for §15. Pure arithmetic -- no codec.

#include "bitrate_budget.h"

#include <algorithm>
#include <cmath>

namespace smartvideo {

namespace {

constexpr int32_t kMaxBackgroundExtra = 30;
constexpr int32_t kMaxRoiSoften = 20;
/// Need a short window of output before reacting, or the first I-frame alone would
/// always look like an overshoot.
constexpr double kMinSeconds = 0.5;
/// How far back the average looks. A lifetime average never recovers from a hot start.
constexpr double kAvgWindowSec = 2.0;
/// Peak window -- aligned with TZ v3's ~1 s rolling peak check.
constexpr double kPeakWindowSec = 1.0;
constexpr double kAvgStepSec = 0.25;
constexpr double kPeakStepSec = 0.10;

} // namespace

BitrateBudget::BitrateBudget(const RateControlPolicy& policy)
    : m_target_bps(policy.target_bitrate)
    , m_critical_importance(policy.critical_importance)
{
    // Zero on the wire means "use the built-in default", not "zero tolerance".
    m_avg_overshoot = policy.max_average_overshoot > 0.0f
        ? policy.max_average_overshoot
        : 0.1f;
    // v3 acceptance: 1 s rolling peak ≤ 120% of target → default overshoot 0.2.
    m_peak_overshoot = policy.max_peak_overshoot > 0.0f
        ? policy.max_peak_overshoot
        : 0.2f;
    m_recovery_sec = policy.recovery_window_ms > 0
        ? static_cast<double>(policy.recovery_window_ms) / 1000.0
        : 1.0;
}

void BitrateBudget::trim(std::vector<Chunk>& chunks, double& sec, int64_t& bytes, double limit)
{
    while (sec > limit && chunks.size() > 1) {
        sec -= chunks.front().duration;
        bytes -= chunks.front().bytes;
        chunks.erase(chunks.begin());
    }
}

void BitrateBudget::step_up()
{
    // How far above the average ceiling we are decides the step size: a mild miss
    // walks the ladder; a large miss (typical when ROI offsets fight CBR) jumps.
    int32_t steps = 1;
    if (m_avg_sec > 0.0 && m_target_bps > 0) {
        const double avg_bps =
            (static_cast<double>(m_avg_bytes) * 8.0) / m_avg_sec;
        const double ratio = avg_bps / static_cast<double>(m_target_bps);
        if (ratio > 1.5) {
            steps = 4;
        } else if (ratio > 1.2) {
            steps = 2;
        }
    }
    for (int32_t i = 0; i < steps; ++i) {
        if (m_background_extra < kMaxBackgroundExtra) {
            ++m_background_extra;
        } else if (m_roi_soften < kMaxRoiSoften) {
            ++m_roi_soften;
        } else {
            break;
        }
    }
}

void BitrateBudget::step_down()
{
    if (m_roi_soften > 0) {
        --m_roi_soften;
    } else if (m_background_extra > 0) {
        --m_background_extra;
    }
}

void BitrateBudget::observe(int64_t packet_bytes, double duration_sec)
{
    if (!active() || packet_bytes <= 0 || duration_sec <= 0.0) {
        return;
    }
    const Chunk c{duration_sec, packet_bytes};

    m_avg_chunks.push_back(c);
    m_avg_sec += duration_sec;
    m_avg_bytes += packet_bytes;
    trim(m_avg_chunks, m_avg_sec, m_avg_bytes, kAvgWindowSec);

    m_peak_chunks.push_back(c);
    m_peak_sec += duration_sec;
    m_peak_bytes += packet_bytes;
    trim(m_peak_chunks, m_peak_sec, m_peak_bytes, kPeakWindowSec);

    m_total_sec += duration_sec;

    // Instantaneous surplus feeds the recovery debt. Undershoot pays it back.
    const double target = static_cast<double>(m_target_bps);
    const double instant_bps = (static_cast<double>(packet_bytes) * 8.0) / duration_sec;
    m_debt_bits = std::max(0.0, m_debt_bits + (instant_bps - target) * duration_sec);

    recompute();
}

void BitrateBudget::recompute()
{
    if (!active() || m_total_sec < kMinSeconds || m_avg_sec <= 0.0) {
        return;
    }

    const double target = static_cast<double>(m_target_bps);
    const double avg_bps =
        (static_cast<double>(m_avg_bytes) * 8.0) / m_avg_sec;
    const double avg_hi = target * (1.0 + static_cast<double>(m_avg_overshoot));
    const double avg_lo = target * (1.0 - static_cast<double>(m_avg_overshoot));

    double peak_bps = 0.0;
    if (m_peak_sec > 0.0) {
        peak_bps = (static_cast<double>(m_peak_bytes) * 8.0) / m_peak_sec;
    }
    const double peak_hi = target * (1.0 + static_cast<double>(m_peak_overshoot));

    // Peak reacts faster: a short burst (I-frame cluster) should climb the ladder before
    // the 2 s average has moved.
    if (peak_bps > peak_hi && m_total_sec - m_last_peak_step_sec >= kPeakStepSec) {
        m_last_peak_step_sec = m_total_sec;
        step_up();
    }

    if (m_total_sec - m_last_avg_step_sec < kAvgStepSec) {
        return;
    }
    m_last_avg_step_sec = m_total_sec;

    if (avg_bps > avg_hi) {
        step_up();
    } else if (m_debt_bits > 0.0) {
        // Still owing bits: do not relax. If we cannot clear the debt inside the recovery
        // window at the current undershoot rate, climb further.
        const double repay_rate = std::max(0.0, target - avg_bps);
        if (repay_rate <= 1.0 || (m_debt_bits / repay_rate) > m_recovery_sec) {
            step_up();
        }
    } else if (avg_bps < avg_lo) {
        step_down();
    }
}

Quality BitrateBudget::apply(const Quality& base) const
{
    Quality out = base;
    if (m_background_extra != 0) {
        out.background_qp_delta = std::clamp(
            out.background_qp_delta + m_background_extra, -51, 51);
    }
    return out;
}

void BitrateBudget::soften_detections(std::vector<Detection>& detections) const
{
    if (m_roi_soften <= 0) {
        return;
    }
    for (Detection& d : detections) {
        if (d.importance >= m_critical_importance) {
            continue;
        }
        // Ceiling at the current background extra so softened ROI does not out-crush the
        // background it is meant to free bits for.
        const int32_t ceiling = std::max(m_background_extra, 0);
        d.qp_delta = std::min(ceiling, d.qp_delta + m_roi_soften);
        d.pad_qp_delta = d.qp_delta / 2;
    }
}

} // namespace smartvideo
