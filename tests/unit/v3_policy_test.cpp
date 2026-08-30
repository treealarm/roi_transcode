// ObjectPriority ladder (v3 §11.1), geometry policy (§11.2–11.3), QP slew / smoothing (§13).

#include "box_smoother.h"
#include "importance.h"
#include "object_normalizer.h"
#include "qp_slew.h"
#include "roi_lag.h"
#include "sve/object_priority.h"
#include "sve/policy.h"
#include "sve/types.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

void check_near(double got, double want, double tol, const std::string& what)
{
    if (std::fabs(got - want) > tol) {
        std::fprintf(stderr, "FAIL: %s (got %.4f, want %.4f)\n", what.c_str(), got, want);
        ++failures;
    }
}

void ladder_maps_kinds_to_canonical_deltas()
{
    check_eq(sve::qp_delta_for(sve::ObjectPriority::Critical), -8, "critical");
    check_eq(sve::qp_delta_for(sve::ObjectPriority::High), -6, "high");
    check_eq(sve::qp_delta_for(sve::ObjectPriority::Normal), -4, "normal");
    check_eq(sve::qp_delta_for(sve::ObjectPriority::Low), -2, "low");
    check_eq(sve::qp_delta_for(sve::ObjectPriority::Background), 0, "background");

    check(sve::priority_from_importance(1.00f) == sve::ObjectPriority::Critical,
        "plate importance is Critical");
    check(sve::priority_from_importance(0.80f) == sve::ObjectPriority::High,
        "person importance is High");
    check(sve::priority_from_importance(0.60f) == sve::ObjectPriority::Normal,
        "vehicle importance is Normal");
}

void detections_use_ladder_by_default()
{
    sve::Quality q;
    q.use_v3_priority_ladder = true;
    q.margin_mode = sve::MarginMode::FixedPixels;
    q.margin_px = 0; // isolate QP from geometry
    q.min_roi_side = 0;
    sve::RoiMapBuilder builder;
    sve::Box plate;
    plate.kind = "license_plate";
    plate.x = 0.1f;
    plate.y = 0.1f;
    plate.width = 0.2f;
    plate.height = 0.1f;
    const auto dets = builder.detections_from({plate}, q, 1280, 720);
    check(dets.size() == 1, "one detection");
    check_eq(dets[0].qp_delta, -8, "plate lands on Critical −8");
    check(dets[0].priority == sve::ObjectPriority::Critical, "priority Critical");
}

void explicit_priority_on_the_box_wins_over_kind()
{
    sve::Quality q;
    q.margin_mode = sve::MarginMode::FixedPixels;
    q.margin_px = 0;
    q.min_roi_side = 0;
    sve::Box b;
    b.kind = "vehicle"; // would be Normal (−4)
    b.has_priority = true;
    b.priority = sve::ObjectPriority::Critical;
    b.x = 0.1f;
    b.y = 0.1f;
    b.width = 0.2f;
    b.height = 0.2f;
    sve::RoiMapBuilder builder;
    const auto dets = builder.detections_from({b}, q, 640, 480);
    check_eq(dets[0].qp_delta, -8, "explicit Critical beats vehicle kind");
}

void min_roi_side_expands_a_tiny_box()
{
    sve::Quality q;
    q.margin_mode = sve::MarginMode::FixedPixels;
    q.margin_px = 0;
    q.min_roi_side = 64;
    sve::Box tiny;
    tiny.kind = "license_plate";
    tiny.x = 0.5f;
    tiny.y = 0.5f;
    tiny.width = 10.0f / 1280.0f;
    tiny.height = 10.0f / 720.0f;
    sve::RoiMapBuilder builder;
    const auto dets = builder.detections_from({tiny}, q, 1280, 720);
    check(dets.size() == 1, "one detection");
    check(dets[0].x2 - dets[0].x1 >= 64, "width at least 64");
    check(dets[0].y2 - dets[0].y1 >= 64, "height at least 64");
}

void fixed_margin_grows_the_body()
{
    sve::Quality q;
    q.margin_mode = sve::MarginMode::FixedPixels;
    q.margin_px = 8;
    q.min_roi_side = 0;
    sve::Box b;
    b.kind = "vehicle";
    b.x = 0.25f;
    b.y = 0.25f;
    b.width = 0.10f;
    b.height = 0.10f;
    sve::RoiMapBuilder builder;
    const auto dets = builder.detections_from({b}, q, 1000, 1000);
    // 0.25*1000=250, width 100 → without margin 250..350; with 8px → 242..358
    check_eq(dets[0].x1, 242, "left expanded by 8");
    check_eq(dets[0].x2, 358, "right expanded by 8");
}

void qp_slew_limits_a_jump()
{
    sve::QpSlewLimiter slew;
    std::vector<sve::Detection> a(1);
    a[0].track_id = 7;
    a[0].qp_delta = -2;
    slew.apply(a, 2);
    check_eq(a[0].qp_delta, -2, "first sample free");

    std::vector<sve::Detection> b(1);
    b[0].track_id = 7;
    b[0].qp_delta = -8;
    slew.apply(b, 2);
    check_eq(b[0].qp_delta, -4, "jump of 6 is clamped to step 2");
}

void box_smoother_blends_toward_history()
{
    sve::BoxSmoother smooth;
    sve::Box a;
    a.track_id = 3;
    a.kind = "person";
    a.x = 0.10f;
    a.y = 0.20f;
    a.width = 0.10f;
    a.height = 0.40f;
    std::vector<sve::Box> first{a};
    smooth.apply(first, 0.35f, 0.25f);
    check_near(first[0].x, 0.10, 1e-5, "first frame seeds state");

    sve::Box b = a;
    b.x = 0.50f;
    b.width = 0.30f;
    std::vector<sve::Box> second{b};
    smooth.apply(second, 0.35f, 0.25f);
    // 0.35*0.50 + 0.65*0.10 = 0.240
    check_near(second[0].x, 0.35 * 0.50 + 0.65 * 0.10, 1e-4, "temporal EMA on x");
    // 0.25*0.30 + 0.75*0.10 = 0.150
    check_near(second[0].width, 0.25 * 0.30 + 0.75 * 0.10, 1e-4, "spatial EMA on width");
}

void v3_temporal_defaults_match_the_spec()
{
    sve::TemporalROIConfig t;
    check(t.max_missing_frames == 3, "max_missing_frames");
    check_near(t.confidence_decay, 0.85, 1e-6, "confidence_decay");
    check_near(t.spatial_smoothing, 0.25, 1e-6, "spatial_smoothing");
    check_near(t.temporal_smoothing, 0.35, 1e-6, "temporal_smoothing");
    check(t.use_track_prediction, "use_track_prediction");
    check_eq(t.max_qp_delta_per_frame, 2, "max_qp_delta_per_frame");
    check(!t.unimplemented_requested(), "nothing left unimplemented");
}

void roi_lag_acceptance_matches_v3_thresholds()
{
    sve::Box a{.x = 0.1f, .y = 0.1f, .width = 0.2f, .height = 0.2f};
    sve::Box b = a;
    b.x = 0.12f;
    const float iou = sve::box_iou(a, b);
    check(iou >= 0.50f, "near boxes still overlap enough");

    std::vector<sve::RoiLagSample> good;
    for (int32_t i = 0; i < 100; ++i) {
        good.push_back(sve::measure_roi_lag(a, b, i < 95 ? 1 : 2, 0.033));
    }
    check(sve::roi_lag_acceptance(good), "P95/P99 inside §14");

    std::vector<sve::RoiLagSample> bad;
    for (int32_t i = 0; i < 100; ++i) {
        bad.push_back(sve::measure_roi_lag(a, b, 10, 0.033));
    }
    check(!sve::roi_lag_acceptance(bad), "ten-frame lag fails acceptance");
}

} // namespace

int main()
{
    ladder_maps_kinds_to_canonical_deltas();
    detections_use_ladder_by_default();
    explicit_priority_on_the_box_wins_over_kind();
    min_roi_side_expands_a_tiny_box();
    fixed_margin_grows_the_body();
    qp_slew_limits_a_jump();
    box_smoother_blends_toward_history();
    v3_temporal_defaults_match_the_spec();
    roi_lag_acceptance_matches_v3_thresholds();

    if (failures > 0) {
        std::fprintf(stderr, "v3_policy_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("v3_policy_test ok");
    return 0;
}
