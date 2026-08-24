// The importance model of TZ v2.0 §11, and the nested-ROI rule of §12.
//
// Level L1 of §18, which names exactly these: "importance normalization", "QP mapping",
// "nested ROI", "overlap policy". Pure arithmetic -- no codec, no FFmpeg, no media.

#include "importance.h"
#include "object_normalizer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
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

smartvideo::Box box(const std::string& kind, float x, float y, float w, float h,
    float confidence = 0.0f)
{
    smartvideo::Box b;
    b.kind = kind;
    b.x = x;
    b.y = y;
    b.width = w;
    b.height = h;
    b.confidence = confidence;
    return b;
}

// §11's own table.
void semantic_priorities_are_the_specification_s()
{
    using smartvideo::default_semantic_priority;
    check_near(default_semantic_priority("license_plate"), 1.00, 1e-6, "plate priority");
    check_near(default_semantic_priority("face"), 0.95, 1e-6, "face priority");
    check_near(default_semantic_priority("person"), 0.80, 1e-6, "person priority");
    check_near(default_semantic_priority("vehicle"), 0.60, 1e-6, "vehicle priority");
    // Not in §11's table; ours, and deliberately not zero.
    check_near(default_semantic_priority("unknown", 0.5f), 0.50, 1e-6, "unknown priority");
}

// The line §12's worked example defines: vehicle 0.60 -> -4, plate 1.00 -> -10.
void the_qp_line_reproduces_the_specification_s_example()
{
    const smartvideo::ImportancePolicy policy;
    check(smartvideo::qp_from_importance(0.60f, policy) == -4, "vehicle lands on -4");
    check(smartvideo::qp_from_importance(1.00f, policy) == -10, "plate lands on -10");
    // The background of §11 falls out of the same line rather than being set separately.
    check(smartvideo::qp_from_importance(0.05f, policy) == 4, "background lands on +4");
}

// Confidence multiplies in; silence does not.
void confidence_is_a_factor_and_silence_is_not_zero()
{
    const smartvideo::ImportancePolicy policy;
    smartvideo::Object certain = smartvideo::object_from_box(
        box("person", 0.1f, 0.1f, 0.2f, 0.2f, 1.0f), policy);
    smartvideo::Object unsure = smartvideo::object_from_box(
        box("person", 0.1f, 0.1f, 0.2f, 0.2f, 0.5f), policy);
    smartvideo::Object silent = smartvideo::object_from_box(
        box("person", 0.1f, 0.1f, 0.2f, 0.2f), policy);

    check_near(smartvideo::importance_of(certain, policy), 0.80, 1e-5, "certain person");
    check_near(smartvideo::importance_of(unsure, policy), 0.40, 1e-5, "half-sure person");
    // A detector that reports nothing must not have its regions multiplied away: most
    // ONVIF cameras report no confidence at all.
    check_near(smartvideo::importance_of(silent, policy), 0.80, 1e-5, "silent detector");
}

// §12, as the specification draws it: a plate inside a vehicle.
void a_plate_inside_a_vehicle_keeps_the_plate_s_quality()
{
    smartvideo::Quality quality;
    quality.background_qp_delta = 0;   // isolate the two regions
    quality.kinds.clear();            // nothing configured, so §11 decides

    const std::vector<smartvideo::Box> boxes{
        box("vehicle", 0.20f, 0.20f, 0.40f, 0.40f, 1.0f),
        box("license_plate", 0.35f, 0.45f, 0.10f, 0.05f, 1.0f),
    };

    smartvideo::RoiMapBuilder builder;
    const auto dets = builder.detections_from(boxes, quality, 1000, 1000);
    const auto rects = builder.build(1000, 1000, dets, quality.background_qp_delta);

    check(rects.size() >= 2, "both regions produced");
    if (rects.size() < 2) {
        return;
    }

    // Ordered by importance, so the encoder's own first-wins rule is §12's rule.
    check(rects[0].qp_delta == -10, "the plate comes first, at -10");
    check(rects[1].qp_delta == -4, "the vehicle follows, at -4");

    // And the plate's rectangle really is inside the vehicle's, which is what makes this
    // the nested case rather than two regions side by side.
    const smartvideo::Rect& plate = rects[0];
    const smartvideo::Rect& vehicle = rects[1];
    check(plate.left >= vehicle.left && plate.right <= vehicle.right
            && plate.top >= vehicle.top && plate.bottom <= vehicle.bottom,
        "the plate is nested inside the vehicle");
}

// A halo softens the step outside a region; it must never cut into another one.
void a_nested_halo_does_not_degrade_its_parent()
{
    smartvideo::Quality quality;
    quality.background_qp_delta = 0;
    quality.kinds.clear();

    const std::vector<smartvideo::Box> boxes{
        box("person", 0.30f, 0.20f, 0.30f, 0.60f, 1.0f),
        box("face", 0.40f, 0.22f, 0.08f, 0.10f, 1.0f),
    };

    smartvideo::RoiMapBuilder builder;
    const auto dets = builder.detections_from(boxes, quality, 1000, 1000);
    const auto rects = builder.build(1000, 1000, dets, quality.background_qp_delta);

    // Every object rectangle precedes every halo. Interleaved, the face's halo would win
    // over the person's body and the torso beside the head would come out worse than the
    // rest of the person.
    std::size_t last_object = 0;
    std::size_t first_halo = rects.size();
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const bool is_halo = rects[i].qp_delta == dets[0].pad_qp_delta
            || rects[i].qp_delta == dets[1].pad_qp_delta;
        if (is_halo && i < first_halo) {
            first_halo = i;
        }
        if (!is_halo) {
            last_object = i;
        }
    }
    check(first_halo > last_object, "no halo precedes an object rectangle");
}

// An explicit per-kind delta is a decision, not a suggestion.
void a_configured_kind_keeps_its_own_number()
{
    smartvideo::Quality quality;
    quality.background_qp_delta = 0;
    quality.kinds["person"] = smartvideo::KindQuality{-20, 0.0};

    smartvideo::RoiMapBuilder builder;
    const auto dets = builder.detections_from(
        {box("person", 0.2f, 0.2f, 0.3f, 0.3f, 1.0f)}, quality, 1000, 1000);
    check(dets.size() == 1 && dets[0].qp_delta == -20,
        "the configured delta survives the model");
}

} // namespace

int main()
{
    semantic_priorities_are_the_specification_s();
    the_qp_line_reproduces_the_specification_s_example();
    confidence_is_a_factor_and_silence_is_not_zero();
    a_plate_inside_a_vehicle_keeps_the_plate_s_quality();
    a_nested_halo_does_not_degrade_its_parent();
    a_configured_kind_keeps_its_own_number();

    if (failures > 0) {
        std::fprintf(stderr, "importance_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("importance_test ok");
    return 0;
}
