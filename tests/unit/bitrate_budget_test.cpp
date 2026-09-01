// Outer bitrate controller for TZ v2.0 §15. Pure arithmetic -- no codec.

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
/// against a one-second recovery window. The escalation test "(debt / repay_rate) >
/// recovery_sec" was therefore true for all eight of them, and the ladder was pinned at its
/// ceiling -- background_extra 30 with roi_soften 20, which is the whole frame at QP 51,
/// object included -- while the stream underneath was producing nothing at all.
///
/// Measured on the real 36 s clip this came from: twelve consecutive seconds of silence
/// against a 2 Mbps target. Measured here: 22.00 s to let go before the fix, 15.28 s after.
/// The bound below sits between them, so this fails outright if the escalation comes back.
///
/// What it does not claim: 15 s is still slow, and most of it is the ladder itself walking
/// fifty steps at one per 0.25 s. That is tuning, and it is open -- see срез Q.
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

} // namespace

int main()
{
    inactive_without_a_target();
    overshoot_crushes_background_before_roi();
    undershoot_releases_only_after_debt_is_paid();
    a_quiet_stream_is_released_in_bounded_time();
    peak_overshoot_climbs_inside_a_wide_average_deadband();
    apply_and_soften_respect_critical_objects();

    if (failures > 0) {
        std::fprintf(stderr, "bitrate_budget_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("bitrate_budget_test ok");
    return 0;
}
