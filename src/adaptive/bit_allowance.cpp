// Outer bitrate controller. Pure arithmetic -- no codec.

#include "bit_allowance.h"

#include <algorithm>
#include <cmath>

namespace ta_roienc {

namespace {

constexpr int32_t kMaxBackgroundExtra = 30;
constexpr int32_t kMaxRoiSoften = 20;
/// The two knobs end to end: background first, then low-importance ROI. Held as one
/// continuous quantity so a correction can be a size rather than a number of rungs.
constexpr double kMaxLevel = kMaxBackgroundExtra + kMaxRoiSoften;
/// Need a short window of output before reacting, or the first I-frame alone would
/// always look like an overshoot.
constexpr double kMinSeconds = 0.5;
/// How far back the average looks. A lifetime average never recovers from a hot start.
constexpr double kAvgWindowSec = 2.0;
/// Peak window -- the 1 s rolling peak.
constexpr double kPeakWindowSec = 1.0;
constexpr double kAvgStepSec = 0.25;
constexpr double kPeakStepSec = 0.10;
constexpr double kMsPerSec = 1000.0;

/// Quantiser steps per doubling of the rate. Both H.264 and H.265 define QP so that six
/// steps double the quantiser, and rate tracks the quantiser closely enough for this to be
/// the standard first-order model. It is what turns "we are 2.4x over" into "raise the
/// quantiser by about eight", which is the whole point of a proportional loop: the size of
/// the correction comes from the size of the miss instead of from a fixed rung.
constexpr double kQpPerDoubling = 6.0;
/// How much of that correction actually reaches the output. The actuator moves the
/// background and the low-importance regions -- never the critical ones -- so a QP-equivalent
/// correction applied there buys less than the same correction applied to the whole frame.
/// Half is deliberately an underestimate: a gain that is too low converges slowly, a gain
/// that is too high oscillates, and only one of those is a product defect. Measured on a
/// 36 s crowd scene at 2 Mbps.
constexpr double kActuatorShare = 0.5;
/// The most one correction may move the level, in quantiser steps. Bounded authority is
/// what separates a controller from a switch: without it a large miss commands the whole
/// range at once, which is the relay behaviour this replaced.
constexpr double kMaxStepPerTick = 4.0;
/// The same bound for the peak path, which is separate because it fires two and a half
/// times as often (every kPeakStepSec, not every kAvgStepSec): the same number there is a
/// two-and-a-half times larger authority per second.
constexpr double kMaxPeakStepPerTick = 2.0;
/// How much of the tightening rate the loop may use to let go again.
constexpr double kReleaseShare = 0.15;
/// The debt may not pull the effective target below half the real one. Repaying an
/// overshoot by silencing the stream is worse than repaying it slowly, and a burst leaves
/// a debt many times the target: without this floor the aim point goes to zero and the
/// output with it.
constexpr double kMinEffectiveTargetFraction = 0.5;
/// How long the loop must sit at its ceiling, still above the band, before the target is
/// called unreachable. Long enough that an ordinary burst does not trip it -- a saturated
/// loop for a moment is a loop doing its job -- and short enough to be in the report of a
/// clip anyone would measure.
constexpr double kUnreachableSec = 2.0;
/// How close to the ceiling still counts as being at it. One quantiser step: the loop
/// keeps making corrections while it is out of authority, and being one step down from the
/// top is not the same as having room.
constexpr double kCeilingTolerance = 1.0;

} // namespace

BitAllowance::BitAllowance(const RateControlPolicy& policy)
    : m_target_bps(policy.target_bitrate)
    , m_critical_importance(policy.critical_importance)
{
    // Zero on the wire means "use the built-in default", not "zero tolerance".
    m_avg_overshoot = policy.max_average_overshoot > 0.0f
        ? policy.max_average_overshoot
        : kDefaultAverageOvershoot;
    // 1 s rolling peak ≤ 120% of target → default overshoot 0.2.
    m_peak_overshoot = policy.max_peak_overshoot > 0.0f
        ? policy.max_peak_overshoot
        : kDefaultPeakOvershoot;
    m_recovery_sec = static_cast<double>(policy.recovery_window_ms > 0
        ? policy.recovery_window_ms
        : kDefaultRecoveryWindowMs) / kMsPerSec;
}

void BitAllowance::trim(std::vector<Piece>& chunks, double& sec, int64_t& bytes, double limit)
{
    while (sec > limit && chunks.size() > 1) {
        sec -= chunks.front().duration;
        bytes -= chunks.front().bytes;
        chunks.erase(chunks.begin());
    }
}

void BitAllowance::move_level(double quantiser_steps, double bound)
{
    // Asymmetric on purpose, and asymmetric in the *gain* rather than in the bound. Being
    // under the target costs background quality; being over costs the promise, so the loop
    // tightens at full gain and lets go at kReleaseShare of it. A loop that releases as
    // eagerly as it clamps turns every lull in the content into the next overshoot, which
    // is where the oscillation in the first proportional version came from: P95 170% of
    // target against the relay's 131%, on the same clip.
    //
    // Scaling the gain and not the bound is what keeps that from becoming a different
    // defect. A fixed slow release rate is slow for every miss alike, so a stream that goes
    // quiet after a burst stays suppressed for twenty seconds -- measured. Scaled, a
    // fifteen-percent undershoot moves the level by a tenth of a step and a stream at two
    // percent of target moves it by two and a half: gentle near the target, decisive far
    // from it.
    const double gained = quantiser_steps < 0.0
        ? quantiser_steps * kReleaseShare
        : quantiser_steps;
    const double bounded = std::clamp(gained, -bound, bound);
    m_level = std::clamp(m_level + bounded, 0.0, kMaxLevel);

    // Background first, ROI only once the background has nothing left to give. That order
    // is the object-first promise and it is why the two knobs are one line and not two
    // loops.
    m_background_extra = static_cast<int32_t>(std::lround(std::min(
        m_level, static_cast<double>(kMaxBackgroundExtra))));
    m_roi_soften = static_cast<int32_t>(std::lround(std::max(
        0.0, m_level - static_cast<double>(kMaxBackgroundExtra))));
}

/// The correction one measured rate asks for, in quantiser steps.
double BitAllowance::correction_for(double measured_bps, double aim_bps) const
{
    if (measured_bps <= 0.0 || aim_bps <= 0.0) {
        return 0.0;
    }
    return kActuatorShare * kQpPerDoubling * std::log2(measured_bps / aim_bps);
}

void BitAllowance::observe(int64_t packet_bytes, double duration_sec)
{
    if (!active() || packet_bytes <= 0 || duration_sec <= 0.0) {
        return;
    }
    const Piece c{duration_sec, packet_bytes};

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
    //
    // Not before the loop is allowed to act, though. The opening of every encode runs hot:
    // the first I-frame is unavoidable and until packets arrive nothing here knows what the
    // content costs. Charging that to the debt makes the loop repay bits no setting of it
    // could have saved, and it repays them the only way it can -- by suppressing the frames
    // right after the start. Measured on a 36 s crowd scene at 2 Mbps: six seconds of near
    // silence behind the opening burst, which is a visible hole and not a rate problem.
    const double target = static_cast<double>(m_target_bps);
    if (m_total_sec >= kMinSeconds) {
        const double instant_bps = (static_cast<double>(packet_bytes) * 8.0) / duration_sec;
        m_debt_bits = std::max(0.0, m_debt_bits + (instant_bps - target) * duration_sec);
        // Capped at what one recovery window can actually give back at the deepest aim the
        // floor allows. A debt larger than that is not a debt this mechanism can work off:
        // the bits are already in the file, and carrying the remainder turns a transient
        // into a permanent penalty. Measured on a 36 s clip at a 300 kbps target: two
        // opening seconds of burst left a debt the loop was still repaying at the end, so
        // it sat at its ceiling for the whole encode and delivered 48% of the target. The
        // overshoot is to be given back within recovery_window_ms, not paid for by the rest
        // of the stream.
        m_debt_bits = std::min(m_debt_bits,
            target * m_recovery_sec * (1.0 - kMinEffectiveTargetFraction));
    }

    recompute();
}

void BitAllowance::recompute()
{
    if (!active() || m_total_sec < kMinSeconds || m_avg_sec <= 0.0) {
        return;
    }

    const double target = static_cast<double>(m_target_bps);
    const double avg_bps = (static_cast<double>(m_avg_bytes) * 8.0) / m_avg_sec;
    const double avg_hi = target * (1.0 + static_cast<double>(m_avg_overshoot));
    const double avg_lo = target * (1.0 - static_cast<double>(m_avg_overshoot));

    // Peak reacts faster: a short burst (an I-frame cluster) should be answered before the
    // 2 s average has moved. Same arithmetic, aimed at the peak ceiling rather than the
    // target, and one-directional -- releasing is the average's decision.
    const double peak_hi = target * (1.0 + static_cast<double>(m_peak_overshoot));
    if (m_peak_sec > 0.0 && m_total_sec - m_last_peak_step_sec >= kPeakStepSec) {
        const double peak_bps = (static_cast<double>(m_peak_bytes) * 8.0) / m_peak_sec;
        if (peak_bps > peak_hi) {
            m_last_peak_step_sec = m_total_sec;
            move_level(correction_for(peak_bps, peak_hi), kMaxPeakStepPerTick);
        }
    }

    if (m_total_sec - m_last_avg_step_sec < kAvgStepSec) {
        return;
    }
    m_last_avg_step_sec = m_total_sec;

    // Inside the band the loop holds. The band is the caller's stated tolerance
    // (`max_average_overshoot`), and holding inside it is what stops a controller from
    // dithering around a target it has already met.
    if (avg_bps <= avg_hi && avg_bps >= avg_lo) {
        return;
    }

    // Outside it, the correction is proportional to how far outside -- and it aims at the
    // target itself, not at the edge of the band: an edge the loop has reached is an edge
    // it stops correcting from.
    //
    // An outstanding debt lowers the aim, so that the overshoot is given back over
    // `recovery_window_ms` rather than merely stopped. Continuous, and floored: a debt
    // turned into a separate escalation rule would be re-armed every GOP by a bursty
    // stream, and the loop would sit at its ceiling with the stream underneath producing
    // nothing.
    double aim = target;
    if (m_debt_bits > 0.0 && m_recovery_sec > 0.0) {
        aim = std::max(target * kMinEffectiveTargetFraction,
            target - m_debt_bits / m_recovery_sec);
    }
    move_level(correction_for(avg_bps, aim), kMaxStepPerTick);

    // The loop has spent its whole range and the stream is still over the band. Held
    // for a while before it is reported, and never taken back: what the caller needs to
    // know is that it happened during this encode, not whether it is happening right now.
    // Time spent with nothing left to give, added up rather than required to be
    // continuous. A loop that is out of authority still works: it comes a step off the
    // ceiling whenever the content gives it a cheap moment and goes straight back. Asking
    // for an unbroken stretch measures how steady the content is, not whether the target is
    // reachable -- measured on a real encode, the loop sat at its ceiling for most of 36
    // seconds and never once for two of them in a row.
    if (avg_bps > avg_hi && m_level >= kMaxLevel - kCeilingTolerance) {
        m_saturated_sec += kAvgStepSec;
        if (m_saturated_sec >= kUnreachableSec) {
            m_unreachable = true;
        }
    }
}

QualityPlan BitAllowance::apply(const QualityPlan& base) const
{
    QualityPlan out = base;
    if (m_background_extra != 0) {
        out.background_qp_delta = std::clamp(
            out.background_qp_delta + m_background_extra, kMinQpDelta, kMaxQpDelta);
    }
    return out;
}

void BitAllowance::soften_detections(std::vector<Sighting>& detections) const
{
    if (m_roi_soften <= 0) {
        return;
    }
    for (Sighting& d : detections) {
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

} // namespace ta_roienc
