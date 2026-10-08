// Outer bitrate controller. Pure arithmetic -- no codec.

#include "bitrate_budget.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int32_t failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

void check_eq(int32_t got, int32_t want, const std::string& what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what.c_str(), got, want);
        ++failures;
    }
}

void inactive_without_a_target()
{
    const sve::BitrateBudget b;
    check(!b.active(), "default budget is inactive");
    check_eq(b.background_extra(), 0, "no background extra");
    check_eq(b.roi_soften(), 0, "no ROI soften");
}

void overshoot_crushes_background_before_roi()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    policy.max_average_overshoot = 0.1f;
    sve::BitrateBudget b(policy);
    check(b.active(), "a target activates the budget");

    for (int32_t i = 0; i < 4; ++i) {
        b.observe(62500, 0.25); // 2 Mbps
    }
    check(b.background_extra() > 0, "overshoot raises background QP");
    check_eq(b.roi_soften(), 0, "ROI still untouched while background can climb");

    for (int32_t i = 0; i < 80; ++i) {
        b.observe(62500, 0.25);
    }
    check_eq(b.background_extra(), 30, "background extra saturates");
    check(b.roi_soften() > 0, "ROI softens only after background is maxed");
}

void undershoot_releases_only_after_debt_is_paid()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    policy.recovery_window_ms = 1000;
    sve::BitrateBudget b(policy);
    for (int32_t i = 0; i < 100; ++i) {
        b.observe(62500, 0.25); // hot
    }
    check(b.roi_soften() > 0 && b.background_extra() > 0, "precondition: both extras up");
    check(b.debt_bits() > 0.0, "hot run leaves a recovery debt");

    const int32_t soft_before = b.roi_soften();
    const int32_t bg_before = b.background_extra();
    // A short undershoot cannot clear a large debt -- extras must not fall yet.
    for (int32_t i = 0; i < 4; ++i) {
        b.observe(15625, 0.25); // 0.5 Mbps
    }
    check(b.roi_soften() >= soft_before && b.background_extra() >= bg_before,
        "debt blocks release while still owing");

    // Long cold stretch pays the debt, then the ladder walks back down.
    for (int32_t i = 0; i < 200; ++i) {
        b.observe(15625, 0.25);
    }
    check(b.debt_bits() < 1.0, "debt is eventually cleared");
    check(b.roi_soften() < soft_before || b.background_extra() < bg_before,
        "extras fall once the debt is gone");
}

/// After a burst, a stream that has gone quiet must be let go in bounded time.
///
/// This is the case the four tests around it cannot see. They feed uniform streams, and the
/// defect needs the sequence a real clip has: a hot opening -- every encode has one, the
/// controller cannot know what the content costs until packets arrive -- followed by the
/// suppression it caused.
///
/// The recovery debt is fed per packet, so four seconds at three times the target leaves
/// sixteen megabits owing, and an undershoot repays it at the target rate: eight seconds,
/// against a one-second recovery window. An escalation rule "(debt / repay_rate) >
/// recovery_sec" would be true for all eight of them and pin the loop at its ceiling --
/// the whole frame at QP 51, object included -- while the stream underneath produced
/// nothing at all. On the real 36 s clip that meant twelve consecutive seconds of silence
/// against a 2 Mbps target; here, 22.00 s to let go with the escalation and 15.28 s
/// without. The bound below sits between them, so this fails outright if it comes back.
///
/// The release is proportional too, so how fast the loop lets go depends on how far under
/// the target the stream has fallen. A stream at two percent of target is released in a
/// few seconds; a stream fifteen percent under barely moves, which is what stops the loop
/// oscillating.
void a_quiet_stream_is_released_in_bounded_time()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 2'000'000;
    policy.recovery_window_ms = 1000;
    sve::BitrateBudget b(policy);

    constexpr double dt = 1.0 / 25.0;
    for (int32_t i = 0; i < 100; ++i) {
        b.observe(30'000, dt);            // 4 s at 6 Mbps -- three times the target
    }
    check_eq(b.background_extra(), 30, "precondition: the burst pins the background");
    check(b.roi_soften() > 0, "precondition: and reaches the ROI too");
    check(b.debt_bits() > 0.0, "precondition: the burst leaves a debt");

    double released_at = 0.0;
    for (int32_t i = 0; i < 25 * 60; ++i) {
        b.observe(200, dt);               // ~40 kbps: two percent of the target
        if (b.background_extra() == 0 && b.roi_soften() == 0) {
            released_at = (i + 1) * dt;
            break;
        }
    }
    check(released_at > 0.0, "the ladder comes back down at all");
    check(released_at < 18.0,
        "and does so without waiting out a debt no undershoot can repay in one second");
}

void peak_overshoot_climbs_inside_a_wide_average_deadband()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    policy.max_average_overshoot = 0.5f; // ±50%: a brief burst stays inside the avg band
    policy.max_peak_overshoot = 0.2f;    // 1.2 Mbps peak ceiling
    sve::BitrateBudget b(policy);

    for (int32_t i = 0; i < 12; ++i) {
        b.observe(31250, 0.25); // 1 Mbps -- on target
    }
    check_eq(b.background_extra(), 0, "on-target leaves the ladder alone");

    // 3 Mbps for one second: average over 2 s is ~2 Mbps (inside ±50%), peak is 3 Mbps.
    for (int32_t i = 0; i < 10; ++i) {
        b.observe(37500, 0.1);
    }
    check(b.background_extra() > 0, "peak overshoot climbs even when average is in band");
}

void apply_and_soften_respect_critical_objects()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    policy.critical_importance = 0.95f;
    sve::BitrateBudget b(policy);
    for (int32_t i = 0; i < 100; ++i) {
        b.observe(62500, 0.25);
    }
    check(b.background_extra() > 0 && b.roi_soften() > 0, "precondition: extras up");

    sve::Quality q;
    q.background_qp_delta = 6;
    const auto adjusted = b.apply(q);
    check(adjusted.background_qp_delta == 6 + b.background_extra(),
        "apply raises background by the extra");
    check(q.background_qp_delta == 6, "apply does not mutate the original");

    std::vector<sve::Detection> dets(2);
    dets[0].importance = 1.00f;
    dets[0].qp_delta = -12;
    dets[0].pad_qp_delta = -6;
    dets[1].importance = 0.60f;
    dets[1].qp_delta = -6;
    dets[1].pad_qp_delta = -3;
    b.soften_detections(dets);
    check_eq(dets[0].qp_delta, -12, "a critical plate is not softened");
    check(dets[1].qp_delta > -6,
        "a non-critical vehicle is softened (may go positive under pressure)");
}


/// A proportional loop answers a big miss with a big correction and a small miss with a
/// small one. That is the whole difference from the ladder it replaced, which answered both
/// with one rung and let the size of the error decide only how long it kept stepping.
void the_correction_is_sized_from_the_miss()
{
    const auto level_after_one_correction = [](double bps_multiple) {
        sve::RateControlPolicy policy;
        policy.target_bitrate = 1'000'000;
        sve::BitrateBudget b(policy);
        // Two observations to pass kMinSeconds, then the one that carries the miss.
        const int64_t bytes = static_cast<int64_t>(bps_multiple * 1'000'000.0 * 0.25 / 8.0);
        for (int32_t i = 0; i < 3; ++i) {
            b.observe(bytes, 0.25);
        }
        return b.background_extra() + b.roi_soften();
    };

    const int32_t small_miss = level_after_one_correction(1.1);
    const int32_t large_miss = level_after_one_correction(4.0);
    check(large_miss > small_miss,
        "four times over the target commands a larger correction than ten percent over");
    check(small_miss <= 1, "a ten-percent miss is answered by about one quantiser step");
}

/// Releasing is deliberately slower than tightening, and this is where that is stated. It
/// is the property that stops the oscillation: measured on a 36 s crowd scene, a loop that
/// lets go as fast as it clamps down reaches a P95 of 170% of target where this one reaches
/// 114%.
void the_loop_lets_go_more_slowly_than_it_clamps_down()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    sve::BitrateBudget b(policy);

    // 1.5x over for one tick: how far one correction climbs.
    for (int32_t i = 0; i < 3; ++i) {
        b.observe(46875, 0.25);
    }
    const int32_t climbed = b.background_extra() + b.roi_soften();
    check(climbed > 0, "precondition: being over the target climbs");

    // The mirror image: 1/1.5 of the target, the same distance in the other direction.
    sve::RateControlPolicy same = policy;
    sve::BitrateBudget under(same);
    for (int32_t i = 0; i < 3; ++i) {
        under.observe(46875, 0.25);
    }
    const int32_t before = under.background_extra() + under.roi_soften();
    for (int32_t i = 0; i < 1; ++i) {
        under.observe(20833, 0.25);   // 0.67 Mbps
    }
    const int32_t after = under.background_extra() + under.roi_soften();
    check(before - after < climbed,
        "one tick of undershoot gives back less than one tick of overshoot took");
}

/// The plant is not the model. The loop assumes six quantiser steps to a doubling and that
/// half of a correction reaches the output; a real encoder obeys neither exactly. This runs
/// it against a plant with a different exponent and asks only that it converges and stays
/// converged -- which is what separates a controller from a lookup table.
void it_converges_against_a_plant_it_does_not_model_exactly()
{
    constexpr double kTarget = 2'000'000.0;
    constexpr double kFreeRunBps = 8'000'000.0;
    /// The plant: every kPlantStepsPerDoubling quantiser steps halve the rate, and only
    /// kPlantShare of the frame responds at all. Both differ from what the loop assumes.
    constexpr double kPlantStepsPerDoubling = 9.0;
    constexpr double kPlantShare = 0.7;

    sve::RateControlPolicy policy;
    policy.target_bitrate = static_cast<uint64_t>(kTarget);
    sve::BitrateBudget b(policy);

    constexpr double dt = 1.0 / 25.0;
    double worst_late_ratio = 0.0;
    for (int32_t frame = 0; frame < 25 * 30; ++frame) {
        const double level = b.background_extra() + b.roi_soften();
        const double bps = kFreeRunBps
            * std::pow(2.0, -kPlantShare * level / kPlantStepsPerDoubling);
        b.observe(static_cast<int64_t>(bps * dt / 8.0), dt);
        // The last ten seconds: by then a converged loop is holding, not hunting.
        if (frame > 25 * 20) {
            worst_late_ratio = std::max(worst_late_ratio, bps / kTarget);
        }
    }
    check(worst_late_ratio < 1.2,
        "after twenty seconds the plant is held inside the rolling bound");
    check(worst_late_ratio > 0.0, "and the loop did not simply stop the stream");
}

/// A target the loop cannot reach has to be reported, not merely missed. It happens
/// when the expensive part of the frame is inside a protected region -- measured on a crowd
/// scene whose box covers 38% of the frame and all of its detail, moving the background
/// penalty from +6 to +30 changed the rate by four percent.
void an_unreachable_target_is_reported_rather_than_missed_quietly()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    sve::BitrateBudget b(policy);
    check(!b.target_unreachable(), "nothing to report before anything is encoded");

    // Three times the target, and it stays there whatever the loop does: a plant the
    // actuator has no grip on at all.
    constexpr double dt = 1.0 / 25.0;
    for (int32_t i = 0; i < 25 * 10; ++i) {
        b.observe(15'000, dt);
    }
    check(b.background_extra() == 30 && b.roi_soften() == 20,
        "precondition: the loop has spent its whole range");
    check(b.target_unreachable(), "and says so");
}

/// The other half of that claim, and the one that keeps it useful: a burst that the loop
/// does absorb must not be reported as an unreachable target. Saturating for a moment is
/// the loop working, not the loop failing.
void a_burst_the_loop_absorbs_is_not_reported()
{
    sve::RateControlPolicy policy;
    policy.target_bitrate = 1'000'000;
    sve::BitrateBudget b(policy);

    constexpr double dt = 1.0 / 25.0;
    for (int32_t i = 0; i < 25; ++i) {
        b.observe(15'000, dt);            // one second at three times the target
    }
    for (int32_t i = 0; i < 25 * 10; ++i) {
        b.observe(5'000, dt);             // and then on target for ten
    }
    check(!b.target_unreachable(), "a burst that the stream comes back from is not a miss");
}

} // namespace

int main()
{
    inactive_without_a_target();
    overshoot_crushes_background_before_roi();
    undershoot_releases_only_after_debt_is_paid();
    a_quiet_stream_is_released_in_bounded_time();
    peak_overshoot_climbs_inside_a_wide_average_deadband();
    apply_and_soften_respect_critical_objects();
    the_correction_is_sized_from_the_miss();
    the_loop_lets_go_more_slowly_than_it_clamps_down();
    it_converges_against_a_plant_it_does_not_model_exactly();
    an_unreachable_target_is_reported_rather_than_missed_quietly();
    a_burst_the_loop_absorbs_is_not_reported();

    if (failures > 0) {
        std::fprintf(stderr, "bitrate_budget_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("bitrate_budget_test ok");
    return 0;
}
