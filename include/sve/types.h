#pragma once

// The vocabulary both entry points share.

#include "sve/backend_registry.h"
#include "sve/backend_selection.h"
#include "sve/importance.h"
#include "sve/object_priority.h"
#include "sve/policy.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sve {

// The quantiser range, named once because it is asserted in fourteen places and because a
// bare 51 says nothing about which of the several things it could be.
//
// H.264 (ITU-T H.264 §7.4.2.1.1) and HEVC (ITU-T H.265 §7.4.3.2) both quantise 8-bit video
// on 0..51, and every backend here clamps to it: libx264's quant_offsets, libx265's
// quantOffsets, mfxExtEncoderROI::DeltaQP and mfxExtMBQP all take numbers on this scale.
// Ten-bit HEVC extends the range downwards (-12..51) and nothing here relies on that yet;
// when something does, this is the one place it changes.
inline constexpr int32_t kMaxQp = 51;
/// The lowest quantiser this library will *ask* for. Zero is legal in both codecs and means
/// lossless, which is never what a region asks for and is what a zero-filled buffer looks
/// like -- iHD reads a per-block map of zeroes as QP 0. So a coded quantiser starts at one.
inline constexpr int32_t kMinCodedQp = 1;

/// A quality offset, in whole quantiser steps. Negative raises quality, so the range is
/// symmetric: at most the whole scale in either direction.
inline constexpr int32_t kMaxQpDelta = kMaxQp;
inline constexpr int32_t kMinQpDelta = -kMaxQp;

// A ratio, for frame rates and time bases. Spelled out here rather than borrowed from
// FFmpeg so the public headers carry no libavutil types -- see encoder_backend.h.
struct Rational {
    int32_t num = 0;
    int32_t den = 1;
};

// How much time the encoder may spend looking for a better answer.
//
// Five steps of our own, not the encoder's own word. Every backend names its presets
// differently and the lists do not line up -- libx264 has ten from "ultrafast" to
// "placebo", QSV has seven and no "ultrafast" at all. A string
// passed straight through meant the caller had to know which backend would open before
// they could name a preset, which is exactly what Backend::Auto makes unknowable.
//
// So the caller says how much effort they are buying, and each backend spends it in its
// own vocabulary.
enum class Preset : int32_t {
    Fastest,
    Fast,
    Balanced,
    Slow,
    Slowest,
};

const char* preset_name(Preset p);
/// The name back, for a command line or a config file. Unknown text gives Balanced.
Preset preset_from_name(const char* name);

// What comes out. HEVC saves more, but changing codec mid-archive is what breaks a
// player: one HLS playlist stitches many clips, and browsers handle a codec change
// within one badly. Which is why a transcoding pass wants SameAsSource rather than
// either fixed value -- see below.
enum class Codec {
    H264,
    H265,
    /// Encode in whatever the input is already in -- H.264 stays H.264, HEVC stays HEVC, and
    /// anything else becomes H.264, which is the one every consumer can play.
    ///
    /// For a transcoder this is usually what is wanted, and for an archive it is a correctness
    /// requirement rather than a preference: one HLS playlist stitches many clips, and a pass that
    /// re-encodes some of them into a different codec is what leaves two codecs in one playlist.
    ///
    /// Resolved before any backend is asked for anything, so no backend ever sees this value -- the
    /// codec ternaries throughout src/backend read "not H264" as HEVC, and that stays true.
    SameAsSource,
};

/// What the bitstream declares itself to be.
///
/// Auto picks from the pixel format: ten bits in means Main10, eight means Main. Naming one
/// explicitly is how a caller says the pairing matters to them, and a pairing that cannot
/// work is refused at Create rather than discovered in a file nothing will play.
enum class CodecProfile {
    Auto,
    H264Baseline,
    H264Main,
    H264High,
    H265Main,
    H265Main10,
};

/// Which codecs a backend can produce at all, before any device is opened.
///
/// Answerable without opening anything, because that is what selection needs: a backend
/// that could never serve the requested codec has to be skipped before it is asked for a
/// device it may not have. The hardware backends answer yes to both; the software ones
/// answer for their own library only.
///
/// `registry` is which deployment is being asked. It defaults to the process-wide one, so
/// the ordinary caller passes nothing; an application may assemble its own.
///
/// Declared here rather than beside the registry because this is where the question is
/// asked. The class and `default_backend_registry()` come from the header that owns them,
/// included below, so that neither signature exists in two places.
bool backend_supports(Backend kind, Codec codec,
    const BackendRegistry& registry = default_backend_registry());



const char* codec_profile_name(CodecProfile p);

enum class InputCodec {
    // Tightly packed planes in the pixel format named by the frame.
    Raw,
    H264,
    H265,
    Jpeg,
};

// One region worth keeping sharp. Coordinates are normalized 0..1 from the
// top-left corner -- the same convention ta_vms archives boxes in.
struct Point {
    float x = 0;
    float y = 0;
};

struct Box {
    // Seconds from the start of the input. Only meaningful for a file, where
    // boxes lie on a timeline; a submitted frame carries the boxes that are its.
    double t_sec = 0;
    std::string kind;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    /// What the detector knows beyond where the box is -- the inputs the importance model
    /// needs and geometry alone cannot supply.
    ///
    /// `confidence` is one of the five factors directly. `track_id` is what makes temporal
    /// stability computable at all: it is the only thing that says two boxes in different
    /// frames are the same object.
    ///
    /// Zero means "not reported" for both, and that is a distinct state rather than a low
    /// value: a detector that gives no confidence is not one reporting zero, and a box with
    /// no track is not part of any. The importance model treats an unreported confidence as
    /// 1.0 rather than as 0, because refusing to protect anything a silent detector found
    /// would be the wrong reading of silence.
    uint64_t track_id = 0;
    float confidence = 0;
    /// The producer's own, narrower word for the class when it has one ("Truck" under
    /// vehicle). Free-form; `kind` is what decides priority.
    std::string label;

    /// Optional outline, same normalized coordinates as x/y/width/height. When there are at
    /// least three points the encoder protects the polygon rather than the axis-aligned box;
    /// the AABB fields stay filled as the cheap fallback for anything that only draws
    /// rectangles (overlays, area budget).
    std::vector<Point> polygon;

    /// Explicit priority-ladder step from the wire. When set, kind→importance is not used to
    /// pick the step (desired_qp_delta still wins over the ladder's QP).
    bool has_priority = false;
    ObjectPriority priority = ObjectPriority::Normal;

    /// Optional override of the QP delta. When set, the priority ladder's QP is skipped for
    /// this box.
    bool has_desired_qp_delta = false;
    int32_t desired_qp_delta = 0;

    /// Filled by TemporalRoi::at from the track's own history. Not a wire field: a Box that
    /// arrived on gRPC or JSON leaves this empty, and the importance factors that need a
    /// track stay at 1.0 until something has stitched the timeline.
    TrackEvidence track;
};

// How much better than the background one kind of region gets.
struct KindQuality {
    // Negative is sharper.
    int32_t qp_delta = -6;
    // Halo around the box as a fraction of the frame, encoded at half the box's
    // delta, which softens the QP step at the edge.
    double pad = 0.02;
};

/// How the halo / margin around a box is measured.
enum class MarginMode {
    /// Quality::pad as a fraction of the frame (the default, also used by KindQuality).
    FractionOfFrame,
    /// Quality::margin_px fixed pixels on each side (default: 8).
    FixedPixels,
};

// Everything that decides how quality is redistributed. One of these serves
// both boxes and masks: a mask has no per-kind names, so it interpolates
// between the background and `mask_qp_delta_at_255`.
struct Quality {
    // 0 leaves the background alone -- not the same as "unset".
    int32_t background_qp_delta = 6;
    // How long a box keeps applying after its sample, in seconds. Files only.
    double hold_sec = 0.25;
    // Cap on regions handed to the encoder: mfxExtEncoderROI takes at most 256
    // and a VA-API driver usually far fewer.
    int32_t max_regions = 32;
    std::unordered_map<std::string, KindQuality> kinds;

    /// Halo used when a kind has no entry in `kinds` -- the importance path's default. An
    /// explicit KindQuality.pad still wins for that kind. Without this, a pad on the
    /// importance path would have nowhere to attach: per-kind rows are exactly what turn the
    /// model off.
    double pad = 0.02;

    /// FixedPixels uses margin_px; FractionOfFrame keeps the fractional pad.
    MarginMode margin_mode = MarginMode::FixedPixels;
    int32_t margin_px = 8;

    /// After margin, a box shorter than this on either side is expanded symmetrically so
    /// the codec still sees it.
    int32_t min_roi_side = 64;

    // Mask input only: the strength a mask value of 255 asks for, and how many
    // steps the 0..255 range is quantized into. Fewer steps means fewer
    // distinct deltas and so fewer rectangles to spend the budget on.
    int32_t mask_qp_delta_at_255 = -6;
    int32_t mask_levels = 4;

    /// What a region is worth, for kinds `kinds` does not name.
    ///
    /// The two work together rather than competing: a kind with an explicit entry in
    /// `kinds` keeps its qp_delta, because someone said what they wanted and that is the
    /// end of it. Everything else is weighed by the model.
    ///
    /// Which means a caller that registers every kind it has seen, all at one delta, turns
    /// this model off without meaning to -- every region ends up at the same delta and, since
    /// importance is derived back from it, at the same importance, so the importance
    /// ordering and the area budget below both see a flat list.
    ImportancePolicy importance;

    /// How much of the frame may be protected before importance decides who misses out.
    ///
    /// Under a constant quantizer there is no bitrate to hand around, but there is a budget
    /// all the same, and it is area: protection is paid for by the background it is taken
    /// from, so a frame with no background left cannot pay. Measured across a real archive,
    /// a person box averages 41.5% of the frame while a face averages 1.9% -- protect the
    /// person and the file grows, protect the face and it does not.
    ///
    /// Regions are already sorted by importance, so the budget rides on that order: take
    /// each until the next would not fit, and give what does not fit a delta of zero.
    /// Zero, not the background penalty -- a person nobody can afford to sharpen is still
    /// not background, and degrading them would be a worse answer than leaving them be.
    ///
    /// A quarter of the frame by default. Below that the codec still has three quarters to
    /// take bits from; above it the archive says the file grows instead of shrinking.
    ///
    /// 0 protects nothing. Negative is read as zero.
    ///
    /// **1.0 is not "no cap".** What is compared against the budget is the sum of the
    /// regions' areas, and overlapping regions each contribute their own: a face inside a
    /// person spends its area twice. So a frame whose regions cover 60% of it can still
    /// exceed a budget of 1.0. To mean "never refuse anything", give a number larger than
    /// the deepest nesting can reach -- 10 is beyond any plausible one.
    ///
    /// Worth knowing before reaching for it: on real footage this setting moves the encoded
    /// size by under one percent whatever it is set to, including with a single box over
    /// half the frame. It is a fuse, not a dial -- what moves the size is
    /// `background_qp_delta`.
    float max_protected_fraction = 0.25f;
};

struct Settings {
    Backend encoder = Backend::Auto;
    /// Which backends Backend::Auto may try, and in what order. Empty is InvalidArgument
    /// under Auto: the library ships no default order, because the only honest default is
    /// the one whoever deploys the system chooses for their own hardware.
    /// `backend_choice_from_name` fills both fields together from a name.
    ///
    /// It sits beside `encoder` and not on EncoderConfig because the two are one decision:
    /// an order only means anything under Backend::Auto, and every helper that produces one
    /// produces the other with it.
    BackendSelection backend_selection;
    /// H.264 by default: it is the codec every player and every browser handles, and a caller
    /// that has an opinion says so. `SameAsSource` is the one to reach for when transcoding.
    Codec codec = Codec::H264;
    // Constant quantizer / CRF.
    int32_t crf = 28;
    // Keyframe interval in frames; 0 means two seconds.
    int32_t gop = 0;
    Preset preset = Preset::Balanced;
    /// Auto follows the input's bit depth.
    CodecProfile profile = CodecProfile::Auto;
    Quality quality;

    /// Whether a capability the hardware lacks is worked around and announced, or refused
    /// outright.
    FallbackPolicy fallback = FallbackPolicy::Permissive;

    /// Temporal ROI (see TemporalROIConfig) and rate control. Rate control's average target
    /// drives the outer BitrateBudget loop; peak/recovery are still refused at open when
    /// set.
    TemporalROIConfig temporal;
    RateControlPolicy rate_control;
    MetadataConfig metadata;
    // Muxer "movflags", file output only. Empty leaves the container default;
    // a ta_vms archive clip needs fragmented mp4 to stay playable.
    std::string movflags;
};

// What an encoder actually takes: a pixel rectangle and a QP delta. On overlap
// the lowest index wins.
struct Rect {
    int32_t top = 0;
    int32_t bottom = 0;
    int32_t left = 0;
    int32_t right = 0;
    int32_t qp_delta = 0;
};

} // namespace sve
