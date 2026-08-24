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
    /// How many of the producer's frames a region survives without a detection before it is
    /// considered gone.
    ///
    /// Frames, not seconds, and that is what lets one setting serve sources running at
    /// different rates: the duration is this count times the interval measured from a
    /// track's own samples. A camera reporting every 0.2s and a detector reporting every
    /// 1.0s both get three missed frames' worth of patience, which is three very different
    /// numbers of seconds.
    ///
    /// Three by default. Measured on a real archive, single-frame misses dominate -- 80 of
    /// 125 for the detector, 1321 of 1646 for the camera -- and three covers all but a
    /// handful without keeping an object alive long after it has left.
    uint32_t max_missing_frames = 3;
    /// Confidence multiplier applied per missing frame, when a region is being kept alive
    /// past the last time it was seen. Not applied between two known samples: there the
    /// confidence is interpolated like everything else, because both ends are facts.
    float confidence_decay = 1.0f;
    float spatial_smoothing = 0.0f;
    float temporal_smoothing = 0.0f;
    /// Ceiling on how fast a region's QP may move between frames, which is what stops the
    /// flicker §43 measures.
    int32_t max_qp_delta_per_frame = 0;
    /// Extrapolate from the track's motion instead of holding the last box.
    bool use_track_prediction = false;

    /// Whether the caller asked for a part of §13 that does not exist yet.
    ///
    /// max_missing_frames and confidence_decay are implemented and are not named here. The
    /// rest still are: declared-but-ignored is the worst state a setting can be in, because
    /// the caller fills it, nothing happens, and the encode looks like it worked.
    bool unimplemented_requested() const
    {
        return spatial_smoothing != 0.0f || temporal_smoothing != 0.0f
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
