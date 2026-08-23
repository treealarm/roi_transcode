#pragma once

// How quality is spent, and how it behaves over time -- TZ v2.0 §13, §14, §15.
//
// Field names are the specification's. What is implemented is narrower than what is
// declared, and each struct says which is which, because a configuration field that is
// silently ignored is worse than one that is absent: the caller sets it, sees no effect,
// and has no way to tell whether the value or the feature is at fault.

#include <cstdint>

namespace smartvideo {

/// §14. OBJECT_FIRST is the default and the only one implemented; the others are rejected
/// at configure time rather than quietly treated as the default.
enum class QualityPolicy {
    OBJECT_FIRST,
    BALANCED,
    BITRATE_FIRST,
};

const char* quality_policy_name(QualityPolicy p);

/// §13. Keeping a region alive across a detector miss.
///
/// None of this is implemented. Today a box applies for Quality::hold_sec around its own
/// sample and nothing else -- a fixed window, blind to whether the object is still there.
/// The distinction that matters and is not in the specification: for an archive the whole
/// track is already known, so a gap can be interpolated rather than predicted, which is
/// simpler and strictly more accurate. Prediction is for the live path.
struct TemporalROIConfig {
    /// How many frames a region survives without a detection before it is dropped.
    uint32_t max_missing_frames = 0;
    /// Confidence multiplier applied per missing frame.
    float confidence_decay = 1.0f;
    float spatial_smoothing = 0.0f;
    float temporal_smoothing = 0.0f;
    /// Ceiling on how fast a region's QP may move between frames, which is what stops the
    /// flicker §43 measures.
    int32_t max_qp_delta_per_frame = 0;
    /// Extrapolate from the track's motion instead of holding the last box.
    bool use_track_prediction = false;

    /// Whether the caller asked for any of this. All-zero means "unset", and a session
    /// opens; anything else is refused at open with the field named.
    ///
    /// Declared-but-ignored is the worst state a setting can be in: the caller fills it,
    /// nothing happens, and the encode looks like it worked. Refusing costs one comparison
    /// and turns a silent no-op into an answer.
    bool any_set() const
    {
        return max_missing_frames != 0 || confidence_decay != 1.0f
            || spatial_smoothing != 0.0f || temporal_smoothing != 0.0f
            || max_qp_delta_per_frame != 0 || use_track_prediction;
    }
};

/// §15. The bitrate budget, spent on critical objects first.
///
/// Not implemented. The encoder runs at a constant quantizer, and that is not an accident:
/// under CQP a QP offset on one region does not move bits elsewhere, which is what makes
/// per-region deltas mean what they say. A bitrate target reintroduces exactly that
/// coupling, so this becomes a layer above each backend's own rate control rather than a
/// setting passed through to it.
struct RateControlPolicy {
    uint64_t target_bitrate = 0;
    /// Allowed overshoot of the average, as a fraction: 0.1 is ten percent.
    float max_average_overshoot = 0.0f;
    float max_peak_overshoot = 0.0f;
    /// How long the controller has to give back an overshoot.
    uint32_t recovery_window_ms = 0;

    /// As TemporalROIConfig::any_set, and for the same reason. A caller who sets a target
    /// bitrate and gets constant-quantizer output has been misled, not served.
    bool any_set() const
    {
        return target_bitrate != 0 || max_average_overshoot != 0.0f
            || max_peak_overshoot != 0.0f || recovery_window_ms != 0;
    }
};

} // namespace smartvideo
