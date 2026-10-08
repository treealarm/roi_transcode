#pragma once

// How quality is spent, and how it behaves over time.
//
// RateControlPolicy describes an outer loop over CQP/CRF, not native encoder VBR/VBV.

#include <cstdint>
#include <functional>
#include <string_view>

namespace ta_roienc {

/// What to do when the hardware cannot do what was asked.
///
/// The choice is between a documented degradation and an honest refusal, and which one is
/// right depends on the caller. A transcoding service wants the clip encoded even if a few
/// regions had to be dropped; a measurement run wants to know that the thing it is
/// measuring did not happen.
///
/// Permissive still says what it did -- a fallback nobody can see is not allowed -- so the
/// difference is whether the caller is told through a log and a counter, or through a
/// returned Unsupported.
enum class FallbackPolicy {
    Permissive,
    Strict,
};

const char* fallback_policy_name(FallbackPolicy p);

/// Keeping a region alive across a detector miss.
///
/// Between two known samples an archive *interpolates*. Past the last sample, patience
/// (`max_missing_frames`) keeps the region alive; with `use_track_prediction` the box
/// keeps moving from the last known velocity instead of freezing. Live frames also get
/// EMA smoothing (`spatial_smoothing` / `temporal_smoothing`) and a QP step ceiling.
struct TemporalROIConfig {
    /// Whether any of this applies at all.
    ///
    /// False means the boxes the producer supplied, at the times it supplied them, and
    /// nothing else: no patience past the last sample, no bridging of a gap, no prediction,
    /// no smoothing and no slew limit. Everything this struct governs is off together,
    /// because a flag that switched off half of it would be a fourth behaviour nobody
    /// asked for.
    bool enabled = true;

    /// How many of the producer's frames a region survives without a detection before it is
    /// considered gone. Frames, not seconds -- duration is this count times the track's
    /// own sample interval.
    uint32_t max_missing_frames = 3;
    /// Ceiling on that patience, in seconds. Zero means none. Caps a slow producer that
    /// would otherwise buy tens of seconds of stale coverage. Does not override an
    /// explicit Quality::hold_sec.
    double max_hold_sec = 2.0;
    /// Confidence multiplier per missing producer frame while guessing.
    float confidence_decay = 0.85f;
    /// EMA weight on the *new* width/height each frame (0 disables).
    float spatial_smoothing = 0.25f;
    /// EMA weight on the *new* x/y each frame (0 disables).
    float temporal_smoothing = 0.35f;
    /// Max |ΔQP| per frame for one track. Implemented by QpSlewLimiter.
    int32_t max_qp_delta_per_frame = 2;
    /// Extrapolate from last motion when past the final sample (within patience).
    bool use_track_prediction = true;

    // There is no IoU threshold for associating a detection with a track: this library
    // never makes that decision, because identity arrives already made in Box::track_id
    // from whoever ran the detector. Reading such a threshold instead as a floor on how far
    // a predicted box may drift breaks prediction itself -- an object moving at a constant
    // velocity drifts past 0.50 IoU within one interval.
};

/// Where object metadata goes.
///
/// Two channels, because the first one is not always available: a backend may not carry
/// SEI at all, and CPU H.265 turns it off outright. The sidecar is how the metadata still
/// reaches the consumer when the bitstream cannot carry it.
enum class SidecarFormat : int32_t {
    /// No sidecar. The default: a caller who has not asked for one gets no extra work.
    None = 0,
    /// One JSON object per line.
    NDJSON,
};

/// One sidecar record, handed over as it is produced.
///
/// A callback rather than a std::ostream. An ostream in this header would put <ostream> on
/// every consumer's include path for a feature most of them do not use, and a callback
/// wraps one in three lines while the reverse is not true. The library never opens a file:
/// naming and container binding are the application's, and transcode_file is where
/// <output_basename>.roimeta.ndjson is spelled.
using SidecarSink = std::function<void(std::string_view record)>;

/// Whether object metadata is written into the bitstream (SEI), whether a sidecar is
/// written beside it, and how many objects one frame may carry.
struct MetadataConfig {
    /// Embed user-data-unregistered SEI when the backend advertises `sei` (libx264/libx265).
    bool bitstream_sei = true;
    /// Soft cap; lowest-priority objects are dropped first if over.
    uint32_t max_objects = 64;

    /// None unless the caller asks, and asking without a sink is InvalidArgument rather
    /// than a silently discarded stream.
    SidecarFormat sidecar = SidecarFormat::None;
    /// Called once per emitted packet, in emission order, with one complete record and no
    /// trailing newline. Record N corresponds to the Nth emitted packet, so this is driven
    /// from packet delivery rather than from submission -- with lookahead those are neither
    /// the same moment nor the same order.
    SidecarSink sidecar_sink;
};

/// The bitrate budget, spent on critical objects first.
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
///
/// The defaults a zero in RateControlPolicy stands for are named and shared because a zero
/// means "use the built-in default", and that default is read in two places that must
/// agree: the outer BitrateBudget loop, and any backend wiring the policy onto its own rate
/// control.
///
/// 105% of target on average. A loop that stops correcting at the edge of its tolerance
/// band sits at that edge, so this bound is also the steady state.
inline constexpr float kDefaultAverageOvershoot = 0.05f;
/// 120% of target over a ~1 s window.
inline constexpr float kDefaultPeakOvershoot = 0.2f;
inline constexpr uint32_t kDefaultRecoveryWindowMs = 1000;

struct RateControlPolicy {
    /// Target average bitrate in bits per second. Zero leaves CQP/CRF alone.
    uint64_t target_bitrate = 0;
    /// Allowed overshoot of the average, as a fraction: 0.05 is five percent. Zero means
    /// "use the built-in default" (kDefaultAverageOvershoot) once a target is set -- not
    /// "zero tolerance".
    float max_average_overshoot = 0.0f;
    /// Allowed overshoot of the ~1 s peak rate. Zero → default 0.2 (120% of target).
    float max_peak_overshoot = 0.0f;
    /// How long the controller has to give back an overshoot, in milliseconds. Zero → 1000.
    uint32_t recovery_window_ms = 0;
    /// Importance at or above this is never softened by the outer loop. Default covers
    /// faces (0.95) and plates (1.00) from the object-kind table.
    float critical_importance = 0.95f;

    bool any_set() const
    {
        return target_bitrate != 0 || max_average_overshoot != 0.0f
            || max_peak_overshoot != 0.0f || recovery_window_ms != 0;
    }
};

} // namespace ta_roienc
