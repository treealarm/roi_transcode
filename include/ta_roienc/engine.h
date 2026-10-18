#pragma once

// One encoder backend: what it can do, how to configure it, and how frames go through it.
//
// This header is public on purpose: an interface a reader cannot find by opening include/
// is not one. Which means no FFmpeg type may appear below: EngineRun is opaque here
// and defined beside the implementations, and tests/unit/api_surface_test.cpp compiles
// against include/ alone so that a leak stops the build rather than the release.
//
// What the backends actually differ in is opening a device and what the hardware will
// accept. Submitting frames and draining packets is the same libavcodec pair for the
// libavcodec-backed ones, so backend_common carries one implementation they inherit.
//
// Design choices worth knowing before changing this interface:
//
//   * Every method takes a EngineRun&, and Configure() returns one. A backend is
//     therefore a stateless description of a way to encode, and one of them can serve many
//     encodes at once -- a single service transcoding several clips concurrently would
//     otherwise need a backend instance per job, or a lock around a shared one.
//
//   * GetCapabilities takes the output codec, because the answer genuinely differs per
//     codec; see the comment on the method.
//
//   * SubmitPicture takes IPictureStore&: by the time a frame reaches a backend there is
//     exactly one form of it, so there is nothing to be null and nothing to decide.
//
//   * CreatePicture exists so that a caller can write into the encoder's own memory instead
//     of paying a copy for every submitted frame.
//
//   * kind() reports which backend opened under Engine::Auto.

#include "ta_roienc/engine_order.h"
#include "ta_roienc/abilities.h"
#include "ta_roienc/coding_params.h"
#include "ta_roienc/picture_store.h"
#include "ta_roienc/source_format.h"
#include "ta_roienc/access_unit_sink.h"
#include "ta_roienc/transcoder.h"
#include "ta_roienc/verdict.h"
#include "ta_roienc/basics.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ta_roienc {

/// What the caller means to ask of an encoder, for the capabilities that depend on it.
///
/// Not every answer is a property of the hardware. On Intel the per-block QP map and the
/// encoder's own bitrate control exclude each other: mfxExtMBQP carries *absolute*
/// quantisers (iHD does not implement the delta mode), and absolute quantisers mean nothing
/// while a rate controller is moving the global one. So "does this backend have a block
/// map" cannot be answered without knowing whether a target bitrate is coming.
///
/// It has to be answerable, because the adaptive layer changes its own behaviour on the
/// answer: effective_region_budget() drops the rectangle budget entirely once qp_offsets is
/// true. A backend that claimed the map and then opened VBR would be handed a full block
/// grid and would silently discard all but four regions.
///
/// Kept as a struct rather than a second argument so that the next configuration-dependent
/// capability does not change the signature again.
struct AbilityQuery {
    Coding codec = Coding::H265;
    /// The target bitrate the caller intends to set, or zero for none.
    /// Only its zero-ness is read today; it is the whole number so that a backend which
    /// cannot serve a *particular* rate can say so later.
    uint64_t target_bitrate = 0;
};

/// What a backend is being asked to open with. The geometry and timing here do not depend
/// on which backend answers.
///
/// Not the public EncodeOptions: that one carries the caller's whole request, log sink and
/// all, while this carries only what a backend needs to open a codec.
struct EngineSetup {
    /// Geometry, pixel layout, frame rate and time base. Held as one structure rather than
    /// as loose fields so that "what is coming in" cannot be edited while meaning to change
    /// "what should come out": those were neighbours here, and a caller reaching for a
    /// bitrate landed on a resolution.
    ///
    /// `input.time_base` left at den == 0 follows the frame rate, which is what a file
    /// wants; a pushed source usually has its own units instead.
    SourceFormat input;

    Engine kind = Engine::Auto;
    Coding codec = Coding::H265;
    int32_t crf = 28;
    int32_t gop = 0;
    Effort preset = Effort::Balanced;
    /// Auto follows input.pixel_layout's bit depth.
    CodingProfile profile = CodingProfile::Auto;

    RoiPersistence temporal;
    RateControlPolicy rate_control;

    /// Which backends Engine::Auto may consider, and in what order.
    EngineOrder engine_order;
};

/// Per-frame instructions that are not the pixels: where the quality goes, and when the
/// frame belongs. Regions are in pixels, strongest first -- on overlap the lowest index
/// wins, which is what both Intel VPL and libavcodec's QSV wrapper do.
struct EnginePictureControl {
    // A span rather than a vector: the caller already has these contiguously, and a vector
    // here would be an allocation and a copy on every frame -- sixty times a second per
    // stream -- for data nobody keeps. Rather than a pointer and a count, because those
    // are two things a caller can get out of step with each other.
    std::span<const Area> regions;
    /// In the encoder time base.
    int64_t pts = 0;
    /// Optional H.264/H.265 user-data-unregistered SEI (UUID || payload). Empty skips.
    std::span<const uint8_t> sei_unregistered;
};

/// What one session did on its way to producing packets.
struct EngineCounters {
    /// Frames the encoder would not take in the format they arrived in.
    long frames_converted = 0;
    /// Frames that had to be uploaded to device memory first.
    long frames_uploaded = 0;

    /// Frames whose per-block quantiser map ran out of scale -- a cell that wanted a
    /// quantiser outside what the codec has and got the nearest one instead.
    ///
    /// It belongs to the backend because only the backend knows the frame's own quantiser,
    /// and a delta only becomes unreachable once it is added to that. On a path that sends
    /// deltas rather than absolute values it stays zero, which is the honest answer there.
    ///
    /// Without it, asking for more than the scale holds does nothing and says nothing: on
    /// the native oneVPL path at crf 28, a background delta of 30 or of 40 produce
    /// byte-identical output, because 28+30 and 28+40 both clamp to 51.
    long frames_quantiser_clamped = 0;
};

/// One opened encoder. What is inside depends entirely on the backend -- libavcodec's
/// context and surface pool for the four here, an MppCtx or an NvEncoder for a backend
/// that talks to its SDK directly -- so this is a base with nothing in it but the right to
/// be destroyed through a pointer to it.
///
/// It was a single concrete struct full of FFmpeg types until that stopped being tenable:
/// a backend not built on libavcodec would have had to live inside a structure whose every
/// field it leaves unused.
class EngineRun {
public:
    virtual ~EngineRun() = default;
    EngineRun(const EngineRun&) = delete;
    EngineRun& operator=(const EngineRun&) = delete;

protected:
    EngineRun() = default;
};

/// Owns a session the backend opened, and a buffer it handed out.
///
/// unique_ptr with a deleter whose operator() is compiled inside the library, rather than
/// a hand-written handle: same semantics, and the one property that matters is the same
/// too. The library's allocator made these, and on Windows a DLL and its caller do not
/// share one -- so the delete has to happen on our side of that line, and a plain
/// unique_ptr would inline it into the caller's.
///
/// This is the whole answer to who owns what. The library frees exactly what it handed
/// out, through these; an IPictureStore a caller implemented themselves is theirs from
/// beginning to end, and SubmitPicture borrows it for the length of the call.
struct TranscoderDeleter {
    void operator()(EngineRun* session) const noexcept;
};
using TranscoderPtr = std::unique_ptr<EngineRun, TranscoderDeleter>;

struct PictureStoreDeleter {
    void operator()(IPictureStore* buffer) const noexcept;
};
using PictureStorePtr = std::unique_ptr<IPictureStore, PictureStoreDeleter>;

/// What a handle out of a backend is, when the thing handed out has no type this header may
/// name.
///
/// `kind` is not here for extension, it is here for the check. A vendor SDK handle and an
/// FFmpeg AVBufferRef are both a pointer, so an untagged void* would let a caller read one
/// as the other -- and the result of that is a crash somewhere else, not a diagnostic here.
enum class NativeHandleKind {
    None,
    /// An AVBufferRef* holding an AVHWDeviceContext. Owned by the session that handed it
    /// out and valid only while that session lives; a caller who needs it for longer takes
    /// its own reference.
    FfmpegHardwareDevice,
};

struct NativeHandle {
    NativeHandleKind kind = NativeHandleKind::None;
    void* value = nullptr;
};

class IEngine {
public:
    virtual ~IEngine() = default;

    /// Which backend this is. The caller needs it to report what opened: with
    /// Engine::Auto that is the only way to find out.
    virtual Engine kind() const = 0;

    /// What this backend can do for a given output codec.
    ///
    /// The answer genuinely differs per codec: on Alder Lake HEVC encodes through
    /// EncSliceLP with CQP only, and H.264 does not share those limits. One capability set
    /// for both codecs would have to be the intersection, which would understate what the
    /// hardware does.
    virtual EngineAbilities GetCapabilities(const AbilityQuery& query) const = 0;

    /// The same question for a caller with nothing to say about rate control. Non-virtual
    /// on purpose: there is one answer to give and one place to give it, and a backend that
    /// overrode this one instead of the other would be answering a question nobody asks.
    EngineAbilities GetCapabilities(Coding codec) const
    {
        return GetCapabilities(AbilityQuery{codec, 0});
    }

    /// Opens an encoder and returns it. VerdictCode::DeviceUnavailable means "not on this
    /// machine" -- the caller moves to the next backend in the chain, so an absent GPU must
    /// not come back as an error.
    ///
    /// The whole open sequence lives here rather than in the caller, so that the caller
    /// knows nothing about libavcodec and a backend talking to an SDK directly can open
    /// itself.
    virtual Fallible<TranscoderPtr> Configure(const EngineSetup& config) const = 0;

    /// A frame allocated where the encoder can read it without a copy. The caller fills
    /// plane(), calls commit(), and submits it.
    ///
    /// VerdictCode::Unsupported is a legitimate answer: a backend that cannot hand out its
    /// own memory says so, and the caller falls back to submitting planes as usual.
    virtual Fallible<PictureStorePtr> CreatePicture(
        EngineRun& session, MemoryKind memory) const = 0;

    /// Hands one frame to the encoder.
    ///
    /// A reference, not a pointer, and a buffer rather than a struct with three optional
    /// shapes in it. By the time a frame reaches a backend there is exactly one shape --
    /// pixels somewhere it can read them -- so there is nothing here to be null and
    /// nothing to decide. A caller with planes in system memory wraps them in a
    /// LayerStore, which costs nothing and says so at the call site.
    virtual Verdict SubmitPicture(EngineRun& session, IPictureStore& frame,
        const EnginePictureControl& control) const = 0;

    /// Takes one packet if there is one. VerdictCode::Again means "feed more frames",
    /// EndOfStream means there will be no more -- neither is a failure.
    virtual Fallible<AccessUnit> ReceiveAccessUnit(EngineRun& session) const = 0;

    /// Tells the encoder no more frames are coming. Packets still come out through
    /// ReceiveAccessUnit afterwards.
    virtual Verdict Flush(EngineRun& session) const = 0;

    /// What the opened encoder produces, in the public vocabulary.
    ///
    /// On the interface rather than read off the session by the caller: the file pipeline
    /// needs the codec, the geometry, the time base and the extradata to open a muxer, and
    /// a backend on its own SDK has no AVCodecContext to read them from. It answers this
    /// from its own headers.
    virtual Fallible<CodingParams> GetCodingParams(EngineRun& session) const = 0;

    /// What this session did that the caller did not ask for.
    ///
    /// Conversions and uploads are the two that matter: both zero is what zero-copy means,
    /// and without a number there is no way to tell a factory frame that avoided the copy
    /// from one that merely moved it.
    virtual EngineCounters Statistics(const EngineRun& session) const = 0;

    /// Some backends open successfully and only fail on the first frame. Those ask for a
    /// probe: the caller encodes one throwaway frame, and reopens if it worked.
    virtual bool NeedsProbe() const { return false; }

    /// What this session actually settled on.
    ///
    /// Defaulted rather than pure: a backend that cannot read its own settings back says
    /// nothing, and nothing is a truthful answer here -- every field is optional and absent
    /// means "cannot say". A pure virtual would have forced every backend to invent one.
    virtual AppliedSettings GetEffectiveSettings(const EngineRun& session) const
    {
        (void)session;
        return {};
    }

    /// The hardware device this session opened, for a caller that must put frames on the
    /// same device as the encoder.
    ///
    /// One caller needs this and the reason is worth stating: a decoder in front of the
    /// encoder can hand over frames that never leave the GPU, but only if it decodes onto
    /// the device the encoder is already using. Without it the frame comes back to system
    /// memory and goes up again, which costs more than the decode it accelerated.
    ///
    /// Defaulted to nothing, like GetEffectiveSettings above: a CPU backend has no device,
    /// and saying so is a truthful answer rather than a gap every backend must fill.
    virtual NativeHandle HardwareDevice(const EngineRun& session) const
    {
        (void)session;
        return {};
    }
};

/// A backend, borrowed. Never null, never owned, and outliving any caller: the backends are
/// immortal singletons, so there is nothing here for a smart pointer to manage -- unique_ptr
/// would claim an ownership nobody has, and shared_ptr would count references to objects
/// that are never released.
using EngineHandle = std::reference_wrapper<const IEngine>;

/// The backends to try, in order.
///
/// Engine::Auto walks `selection`; a named kind gives just that one and ignores the
/// selection's permissions, because the caller has already been explicit and asking for
/// QSV while getting libx264 is exactly what must not happen silently.
///
/// `codec` is what the caller intends to produce, and backends that cannot produce it are
/// left out: a preference list can name one that has nothing to offer this particular
/// encode. Each one left out appends its reason to `skipped` when that is given -- a
/// backend quietly dropped from the chain would be a silent fallback.
///
/// An empty result means nothing can serve this codec. For a named kind that is
/// Unsupported; for Auto it is an exhausted chain.
///
/// `registry` is the deployment whose backends are walked, defaulting to the process-wide
/// one; an application may assemble its own.
std::vector<EngineHandle> resolve_engines(Engine requested, const EngineOrder& selection,
    Coding codec, std::vector<std::string>* skipped = nullptr,
    const EngineCatalog& registry = default_engine_catalog());

} // namespace ta_roienc
