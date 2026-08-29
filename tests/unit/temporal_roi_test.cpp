// Keeping a region alive across a detector miss -- TZ v2.0 §13.
//
// Level L1 of §18, which names "temporal smoothing" among the unit-testable pieces. Pure
// arithmetic: no codec, no media, no clock.

#include "temporal_roi.h"

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

smartvideo::Box box(double t, uint64_t track, float x, float w = 0.1f)
{
    smartvideo::Box b;
    b.t_sec = t;
    b.kind = "person";
    b.track_id = track;
    b.x = x;
    b.y = 0.2f;
    b.width = w;
    b.height = 0.4f;
    b.confidence = 0.9f;
    return b;
}

// The case §13 opens with: seen, missed, seen again.
void a_gap_is_filled_from_both_ends()
{
    // Samples at 0 and 2 seconds, so the one at 1 is the frame the detector missed.
    const std::vector<smartvideo::Box> boxes{box(0.0, 7, 0.10f), box(2.0, 7, 0.30f)};
    smartvideo::TemporalROIConfig config;   // max_missing_frames 3
    const smartvideo::TemporalRoi roi(boxes, config, 0.5);

    const auto mid = roi.at(1.0);
    check(mid.size() == 1, "the region survives the miss");
    if (mid.size() == 1) {
        // Interpolated, not held: an archive knows both ends, so this is where the object
        // was, not where it was last seen.
        check_near(mid[0].x, 0.20, 1e-4, "position is interpolated across the gap");
    }

    // A quarter of the way along, to make sure it is a line and not a midpoint special case.
    const auto quarter = roi.at(0.5);
    check(quarter.size() == 1 && std::fabs(quarter[0].x - 0.15f) < 1e-4,
        "the interpolation is linear");
}

// One region per track, not one per sample. This is what stops a 5 Hz source from spending
// a region budget of 32 on six objects.
void samples_of_one_track_do_not_pile_up()
{
    std::vector<smartvideo::Box> boxes;
    for (int i = 0; i < 10; ++i) {
        boxes.push_back(box(i * 0.2, 7, 0.10f + 0.01f * static_cast<float>(i)));
    }
    const smartvideo::TemporalRoi roi(boxes, smartvideo::TemporalROIConfig{}, 0.5);
    const auto live = roi.at(1.0);
    check(live.size() == 1, "ten samples of one track give one region, not ten");
}

// An object that walked out of frame must stop being protected. hold_sec cannot tell this
// from a gap; a track can.
void a_track_that_ended_is_let_go()
{
    const std::vector<smartvideo::Box> boxes{box(0.0, 7, 0.10f), box(1.0, 7, 0.20f)};
    smartvideo::TemporalROIConfig config;
    config.max_missing_frames = 2;          // two missed samples of patience
    const smartvideo::TemporalRoi roi(boxes, config, 0.5);

    check(roi.at(2.5).size() == 1, "still held one and a half samples after the last sight");
    check(roi.at(4.0).empty(), "let go once the patience runs out");
}

// The patience is counted in the producer's frames, so one setting serves sources running at
// different rates -- which is the thing a single hold_sec cannot do.
void patience_follows_the_source_s_own_rate()
{
    smartvideo::TemporalROIConfig config;
    config.max_missing_frames = 2;

    // A slow producer: one sample per second. Two frames of patience is two seconds.
    const std::vector<smartvideo::Box> slow{box(0.0, 1, 0.1f), box(1.0, 1, 0.1f)};
    const smartvideo::TemporalRoi slow_roi(slow, config, 0.5);
    check(!slow_roi.at(2.5).empty(), "a 1 Hz source is still held at +1.5s");

    // A fast one: five samples per second. The same two frames is 0.4 seconds.
    const std::vector<smartvideo::Box> fast{box(0.0, 1, 0.1f), box(0.2, 1, 0.1f)};
    const smartvideo::TemporalRoi fast_roi(fast, config, 0.5);
    check(fast_roi.at(1.0).empty(), "a 5 Hz source is let go by +0.8s");
}

// Confidence fades while a region is being kept alive on faith, and does not while both ends
// of the gap are known.
void confidence_decays_only_where_it_is_a_guess()
{
    smartvideo::TemporalROIConfig config;
    config.confidence_decay = 0.5f;
    config.use_track_prediction = false; // hold last box; test is about confidence only

    const std::vector<smartvideo::Box> boxes{box(0.0, 7, 0.1f), box(1.0, 7, 0.2f)};
    const smartvideo::TemporalRoi roi(boxes, config, 0.5);

    check_near(roi.at(0.5)[0].confidence, 0.9, 1e-4,
        "between two known samples the confidence is interpolated, not decayed");
    const auto after = roi.at(2.0);
    check(after.size() == 1 && after[0].confidence < 0.9f * 0.6f,
        "one sample past the end, halved");
}

// Past the last detection the box keeps moving when prediction is on (TZ v3).
void prediction_continues_motion_past_the_last_sample()
{
    // x: 0.10 at t=0, 0.30 at t=2 → at t=3 (one second past end) x = 0.40
    const std::vector<smartvideo::Box> boxes{box(0.0, 7, 0.10f), box(2.0, 7, 0.30f)};
    smartvideo::TemporalROIConfig config;
    config.max_missing_frames = 3;
    config.use_track_prediction = true;
    config.confidence_decay = 1.0f;
    const smartvideo::TemporalRoi roi(boxes, config, 0.5);

    const auto past = roi.at(3.0);
    check(past.size() == 1, "still held with prediction");
    if (past.size() == 1) {
        check_near(past[0].x, 0.40, 1e-4, "position continues along the last velocity");
    }

    config.use_track_prediction = false;
    const smartvideo::TemporalRoi held(boxes, config, 0.5);
    const auto frozen = held.at(3.0);
    check(frozen.size() == 1 && std::fabs(frozen[0].x - 0.30f) < 1e-4,
        "without prediction the last box is frozen");
}

// A source that reports no track has nothing to stitch, and keeps the old behaviour.
void untracked_boxes_keep_the_symmetric_window()
{
    smartvideo::Box b = box(1.0, 0, 0.1f);   // track_id 0 == not reported
    const smartvideo::TemporalRoi roi({&b, 1}, smartvideo::TemporalROIConfig{}, 0.5);
    check(roi.untracked_count() == 1, "an untracked box is kept apart");
    check(roi.at(1.4).size() == 1, "held inside the window");
    check(roi.at(1.6).empty(), "dropped outside it");
}

// §11's last two factors read track age and speed off what at() already knows.
void at_attaches_track_evidence_for_importance()
{
    // Moves 0.20 of the frame in x over 2 s → speed = 0.20 / (√2 * 2) ≈ 0.0707 diagonals/s.
    const std::vector<smartvideo::Box> boxes{box(0.0, 7, 0.10f), box(2.0, 7, 0.30f)};
    const smartvideo::TemporalRoi roi(boxes, smartvideo::TemporalROIConfig{}, 0.5);

    const auto mid = roi.at(1.0);
    check(mid.size() == 1 && mid[0].track.present, "interpolated box carries evidence");
    check_near(mid[0].track.age_sec, 1.0, 1e-5, "age is seconds since the first sample");
    check(mid[0].track.sample_count == 2, "both samples count");
    check_near(mid[0].track.speed, 0.20 / (std::sqrt(2.0) * 2.0), 1e-4,
        "speed is centre displacement in frame-diagonals per second");

    smartvideo::Box lone = box(1.0, 3, 0.5f);
    const smartvideo::TemporalRoi single({&lone, 1}, smartvideo::TemporalROIConfig{}, 0.5);
    const auto one = single.at(1.0);
    check(one.size() == 1 && one[0].track.present && one[0].track.speed == 0.0f,
        "a single-sample track has age but no speed");
}

} // namespace

int main()
{
    a_gap_is_filled_from_both_ends();
    samples_of_one_track_do_not_pile_up();
    a_track_that_ended_is_let_go();
    patience_follows_the_source_s_own_rate();
    confidence_decays_only_where_it_is_a_guess();
    prediction_continues_motion_past_the_last_sample();
    untracked_boxes_keep_the_symmetric_window();
    at_attaches_track_evidence_for_importance();

    if (failures > 0) {
        std::fprintf(stderr, "temporal_roi_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("temporal_roi_test ok");
    return 0;
}
