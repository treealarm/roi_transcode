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

// A class we recognise is worth what its class is worth, however sure the detector was.
void a_known_class_is_not_scaled_by_confidence()
{
    const smartvideo::ImportancePolicy policy;
    for (float c : {1.0f, 0.6f, 0.51f}) {
        const smartvideo::Object person = smartvideo::object_from_box(
            box("person", 0.1f, 0.1f, 0.2f, 0.2f, c), policy);
        check_near(smartvideo::importance_of(person, policy), 0.80, 1e-5,
            "a person is worth 0.80 at confidence " + std::to_string(c));
    }
}

// The property the whole thing exists for: a plate outranks a car and a face outranks the
// body it belongs to, whatever the detector's confidence in each happened to be.
//
// The numbers are this deployment's own, measured over its archive: a face averages 0.633
// confidence against a person's 0.892. Multiplied in -- which is what §11 says literally --
// they invert the table, 0.601 against 0.714, and the body wins.
void the_class_hierarchy_survives_an_unhelpful_detector()
{
    const smartvideo::ImportancePolicy policy;
    const auto face = smartvideo::object_from_box(
        box("face", 0.4f, 0.2f, 0.05f, 0.06f, 0.633f), policy);
    const auto person = smartvideo::object_from_box(
        box("person", 0.3f, 0.2f, 0.3f, 0.6f, 0.892f), policy);
    check(smartvideo::importance_of(face, policy) > smartvideo::importance_of(person, policy),
        "a face outranks a person despite being detected less confidently");

    const auto plate = smartvideo::object_from_box(
        box("license_plate", 0.4f, 0.5f, 0.08f, 0.03f, 0.50f), policy);
    const auto vehicle = smartvideo::object_from_box(
        box("vehicle", 0.2f, 0.2f, 0.5f, 0.5f, 0.95f), policy);
    check(smartvideo::importance_of(plate, policy)
            > smartvideo::importance_of(vehicle, policy),
        "a plate outranks a vehicle despite being detected less confidently");

    // And the letter of §11 is still reachable, for a deployment that wants it.
    smartvideo::ImportancePolicy literal = policy;
    literal.weigh_confidence_on_known_classes = true;
    check(smartvideo::importance_of(face, literal) < smartvideo::importance_of(person, literal),
        "with confidence weighed, the table inverts -- which is why it is off");
}

// A class nobody named has nothing but the detector's word, so there confidence weighs.
void an_unknown_class_is_scaled_by_confidence()
{
    const smartvideo::ImportancePolicy policy;   // unknown_priority 0.5
    const auto sure = smartvideo::object_from_box(
        box("bag", 0.1f, 0.1f, 0.2f, 0.2f, 1.0f), policy);
    const auto unsure = smartvideo::object_from_box(
        box("bag", 0.1f, 0.1f, 0.2f, 0.2f, 0.6f), policy);
    check_near(smartvideo::importance_of(sure, policy), 0.50, 1e-5, "a certain unknown");
    check_near(smartvideo::importance_of(unsure, policy), 0.30, 1e-5, "an unsure unknown");

    // Silence is not zero: most ONVIF cameras report no confidence at all, and reading that
    // as zero would multiply every one of their regions away.
    const auto silent = smartvideo::object_from_box(
        box("bag", 0.1f, 0.1f, 0.2f, 0.2f), policy);
    check_near(smartvideo::importance_of(silent, policy), 0.50, 1e-5, "a silent detector");

    // Even at full confidence an unnamed object stays below the least of the named ones.
    check(smartvideo::importance_of(sure, policy)
            < smartvideo::default_semantic_priority("vehicle"),
        "an unknown never outranks a class we recognise");
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
    const auto rects = builder.build(1000, 1000, dets, quality);

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
    const auto rects = builder.build(1000, 1000, dets, quality);

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

// §15's budget, in area. The case the archive handed us: a person filling nearly half the
// frame, with their face inside it. There is no background left to pay for protecting the
// body, so the body goes to neutral and the face -- 0.8% of the frame and the most valuable
// thing in it -- keeps everything it asked for.
void the_budget_protects_the_face_and_lets_the_body_go()
{
    smartvideo::Quality quality;
    quality.background_qp_delta = 6;
    quality.kinds.clear();
    quality.max_protected_fraction = 0.25f;

    const std::vector<smartvideo::Box> boxes{
        box("person", 0.25f, 0.10f, 0.50f, 0.85f, 1.0f),   // 42.5% of the frame
        box("face", 0.44f, 0.14f, 0.09f, 0.09f, 1.0f),     //  0.8% of the frame
    };

    smartvideo::RoiMapBuilder builder;
    const auto dets = builder.detections_from(boxes, quality, 1000, 1000);
    const auto rects = builder.build(1000, 1000, dets, quality);

    // Face, person, the face's halo, the background -- and nothing else. The person's halo
    // is the one that is missing, because a halo softens a step that no longer exists.
    check(rects.size() == 4, "four rectangles: two objects, one halo, the background");
    if (rects.size() != 4) {
        return;
    }

    // Importance order puts the face first, so it is the one that gets paid for.
    check(rects[0].qp_delta == -9, "the face keeps its own delta");
    check(rects[1].qp_delta == 0,
        "the person is neutral -- not sharpened, and not degraded either");
    check(rects[2].qp_delta == -4, "the halo present is the face's");

    // And the background is still penalised, which is where the saving comes from.
    check(rects[3].qp_delta == 6 && rects[3].left == 0 && rects[3].right == 1000,
        "the background rectangle survives the budget");
}

// With the budget off, nothing changes -- the setting has to be able to get out of the way.
void a_budget_of_zero_changes_nothing()
{
    smartvideo::Quality quality;
    quality.background_qp_delta = 6;
    quality.kinds.clear();

    const std::vector<smartvideo::Box> boxes{
        box("person", 0.25f, 0.10f, 0.50f, 0.85f, 1.0f),
    };

    smartvideo::RoiMapBuilder builder;
    const auto dets = builder.detections_from(boxes, quality, 1000, 1000);

    quality.max_protected_fraction = 0.0f;
    const auto unbudgeted = builder.build(1000, 1000, dets, quality);
    check(unbudgeted.size() == 3 && unbudgeted[0].qp_delta == -7,
        "an unbudgeted person keeps its delta and its halo");

    quality.max_protected_fraction = 0.25f;
    const auto budgeted = builder.build(1000, 1000, dets, quality);
    check(budgeted.size() == 2 && budgeted[0].qp_delta == 0,
        "the same person under a budget goes neutral and loses its halo");
}

} // namespace

int main()
{
    semantic_priorities_are_the_specification_s();
    the_qp_line_reproduces_the_specification_s_example();
    a_known_class_is_not_scaled_by_confidence();
    the_class_hierarchy_survives_an_unhelpful_detector();
    an_unknown_class_is_scaled_by_confidence();
    a_plate_inside_a_vehicle_keeps_the_plate_s_quality();
    a_nested_halo_does_not_degrade_its_parent();
    a_configured_kind_keeps_its_own_number();
    the_budget_protects_the_face_and_lets_the_body_go();
    a_budget_of_zero_changes_nothing();

    if (failures > 0) {
        std::fprintf(stderr, "importance_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("importance_test ok");
    return 0;
}
