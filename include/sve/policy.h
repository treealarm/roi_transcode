#pragma once

// How quality is spent, and how it behaves over time -- TZ v3 §13, §14, §15.
//
// Field names follow the specification. TemporalROIConfig defaults match v3 §13
// (anti-flicker). RateControlPolicy still describes an outer loop over CQP/CRF,
// not native encoder VBR/VBV.

#include <cstdint>

namespace sve {

/// §14. OBJECT_FIRST is the default and the only one implemented; the others are rejected
/// at configure time rather than quietly treated as the default.
enum class QualityPolicy {
    OBJECT_FIRST,
    BALANCED,
    BITRATE_FIRST,
};

const char* quality_policy_name(QualityPolicy p);

/// §13. Keeping a region alive across a detector miss (TZ v3 defaults).
///
/// Between two known samples an archive *interpolates*. Past the last sample, patience
/// (`max_missing_frames`) keeps the region alive; with `use_track_prediction` the box
/// keeps moving from the last known velocity instead of freezing. Live frames also get
/// EMA smoothing (`spatial_smoothing` / `temporal_smoothing`) and a QP step ceiling.
struct TemporalROIConfig {
    /// How many of the producer's frames a region survives without a detection before it is
    /// considered gone. Frames, not seconds -- duration is this count times the track's
    /// own sample interval.
    uint32_t max_missing_frames = 3;
    /// Ceiling on that patience, in seconds. Zero means none. Caps a slow producer that
    /// would otherwise buy tens of seconds of stale coverage. Does not override an
    /// explicit Quality::hold_sec.
    double max_hold_sec = 2.0;
    /// Confidence multiplier per missing producer frame while guessing. TZ v3: 0.85.
    float confidence_decay = 0.85f;
    /// EMA weight on the *new* width/height each frame (0 disables). TZ v3: 0.25.
    float spatial_smoothing = 0.25f;
    /// EMA weight on the *new* x/y each frame (0 disables). TZ v3: 0.35.
    float temporal_smoothing = 0.35f;
    /// Max |ΔQP| per frame for one track. Implemented by QpSlewLimiter. TZ v3: 2.
    int32_t max_qp_delta_per_frame = 2;
    /// Extrapolate from last motion when past the final sample (within patience). TZ v3: true.
    bool use_track_prediction = true;

    /// Always false -- §13 fields above are implemented. Kept for older call sites.
    bool unimplemented_requested() const { return false; }
};

/// TZ v3 §19 -- whether object metadata is written into the bitstream (SEI) and how many
/// objects one frame may carry.
struct MetadataConfig {
    /// Embed user-data-unregistered SEI when the backend advertises `sei` (libx264/libx265).
    bool bitstream_sei = true;
    /// Soft cap; lowest-priority objects are dropped first if over.
    uint32_t max_objects = 64;
};

/// §15. The bitrate budget, spent on critical objects first.
///
/// The encoder itself still runs at a constant quantizer (CQP/CRF): that is what keeps
/// per-region QP deltas honest. A target bitrate is handled by an *outer* loop
/// (`BitrateBudget`) that watches average and peak output bits and, when the stream runs
/// hot, raises the background QP first and only then softens low-importance ROI. Faces and
/// plates (importance ≥ `critical_importance`) are not softened. A recovery debt keeps the
/// extras up until an overshoot has been paid back inside `recovery_window_ms`.
///
/// On CPU (`native_bitrate` capability) a non-zero target opens CBR+filler for H.264.
/// H.265 and hardware backends stay on CRF/CQP and rely on the outer loop alone.
/// BitrateBudget remains an object-first fine-tuner. Area budget
/// (`Quality::max_protected_fraction`) still caps how much of the frame may be protected
/// at once.
struct RateControlPolicy {
    /// Target average bitrate in bits per second. Zero leaves CQP/CRF alone.
    uint64_t target_bitrate = 0;
    /// Allowed overshoot of the average, as a fraction: 0.1 is ten percent. Zero means
    /// "use the built-in default" (also 0.1) once a target is set -- not "zero tolerance".
    float max_average_overshoot = 0.0f;
    /// Allowed overshoot of the ~1 s peak rate. Zero → default 0.2 (120% of target, as in
    /// the v3 acceptance note).
    float max_peak_overshoot = 0.0f;
    /// How long the controller has to give back an overshoot, in milliseconds. Zero → 1000.
    uint32_t recovery_window_ms = 0;
    /// Importance at or above this is never softened by the outer loop. Default covers
    /// faces (0.95) and plates (1.00) from §11's table.
    float critical_importance = 0.95f;

    bool any_set() const
    {
        return target_bitrate != 0 || max_average_overshoot != 0.0f
            || max_peak_overshoot != 0.0f || recovery_window_ms != 0;
    }

    /// All declared §15 fields are handled by BitrateBudget. Nothing left to refuse.
    bool unimplemented_requested() const { return false; }
};

} // namespace sve
