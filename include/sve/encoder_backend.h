#pragma once

// One encoder backend: what it can do, how to configure it, and how frames go through it.
// TZ v2.0 §7.
//
// This header is public on purpose. §7 describes IEncoderBackend as part of the contract,
// and an interface a reader cannot find by opening include/ is not one. Which means no
// FFmpeg type may appear below: BackendSession is opaque here and defined beside the
// implementations, and tests/unit/public_api_test.cpp compiles against include/ alone so
// that a leak stops the build rather than the release.
//
// What the four backends actually differ in is opening a device and what the hardware will
// accept. Submitting frames and draining packets is the same libavcodec pair for all of
// them, so backend_common carries one implementation they inherit -- the interface matches
// the specification without three copies of the same function behind it.
//
// Where this departs from the letter of §7, and why. Named here rather than discovered at
// acceptance:
//
//   * §7's interface is stateful: Configure() then SubmitFrame() on the same object, with
//     the open encoder held inside it. Every method here takes a BackendSession& instead,
//     and Configure() returns one. A backend is therefore a stateless description of a way
//     to encode, and one of them can serve many encodes at once -- which is what this
//     library is for: a single service transcoding several clips concurrently would
//     otherwise need a backend instance per job, or a lock around a shared one.
//
//   * GetCapabilities takes the output codec. §7 spells it without an argument; the answer
//     genuinely differs per codec, see the comment on the method.
//
//   * SubmitFrame takes IFrameBuffer&, where §7 has `const Frame&`. Same idea, stricter
//     shape: by the time a frame reaches a backend there is exactly one form of it, so
//     there is nothing to be null and nothing to decide. See the comment on the method.
//
//   * CreateFrame is an addition. §7 has no frame factory; without one, a caller cannot
//     write into the encoder's own memory and every submitted frame costs a copy.
//
//   * kind() is an addition, for reporting which backend opened under Encoder::Auto.

#include "sve/capabilities.h"
#include "sve/frame_buffer.h"
#include "sve/input_format.h"
#include "sve/packet_handler.h"
#include "sve/session.h"
#include "sve/status.h"
#include "sve/types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sve {

/// What the encoder is being asked to produce. Named EncoderConfig after §7; the geometry
/// and timing here do not depend on which backend answers.
struct EncoderConfig {
    /// Geometry, pixel layout, frame rate and time base -- TZ v3.4 §8.3. Held as one
    /// structure rather than as loose fields so that "what is coming in" cannot be edited
    /// while meaning to change "what should come out": those were neighbours here, and a
    /// caller reaching for a bitrate landed on a resolution.
    ///
    /// `input.time_base` left at den == 0 follows the frame rate, which is what a file
    /// wants; a pushed source usually has its own units instead.
    InputVideoFormat input;

    Encoder kind = Encoder::Auto;
    Codec codec = Codec::H265;
    int32_t crf = 28;
    int32_t gop = 0;
    Preset preset = Preset::Balanced;
    /// §14; only OBJECT_FIRST opens, see RoiEncoder::open_deferred.
    QualityPolicy policy = QualityPolicy::OBJECT_FIRST;

    /// §13 and §15. Declared so a caller has somewhere to put values it already has, and
    /// refused at open unless left at their defaults -- neither is implemented, and
    /// accepting them silently would be the misleading answer.
    TemporalROIConfig temporal;
    RateControlPolicy rate_control;
};

/// Per-frame instructions that are not the pixels: where the quality goes, and when the
/// frame belongs. Regions are in pixels, strongest first -- on overlap the lowest index
/// wins, which is what both Intel VPL and libavcodec's QSV wrapper do.
struct BackendFrameControl {
    // A span rather than a vector: the caller already has these contiguously, and a vector
    // here would be an allocation and a copy on every frame -- sixty times a second per
    // stream -- for data nobody keeps. Rather than a pointer and a count, because those
    // are two things a caller can get out of step with each other.
    std::span<const Rect> regions;
    /// In the encoder time base.
    int64_t pts = 0;
    /// Optional H.264/H.265 user-data-unregistered SEI (UUID || payload). Empty skips.
    std::span<const uint8_t> sei_unregistered;
};

/// One opened encoder. What is inside depends entirely on the backend -- libavcodec's
/// context and surface pool for the four here, an MppCtx or an NvEncoder for a backend
/// that talks to its SDK directly -- so this is a base with nothing in it but the right to
/// be destroyed through a pointer to it.
///
/// It was a single concrete struct full of FFmpeg types until that stopped being tenable:
/// a backend not built on libavcodec would have had to live inside a structure whose every
/// field it leaves unused.
class BackendSession {
public:
    virtual ~BackendSession() = default;
    BackendSession(const BackendSession&) = delete;
    BackendSession& operator=(const BackendSession&) = delete;

protected:
    BackendSession() = default;
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
/// out, through these; an IFrameBuffer a caller implemented themselves is theirs from
/// beginning to end, and SubmitFrame borrows it for the length of the call.
struct SessionDeleter {
    void operator()(BackendSession* session) const noexcept;
};
using SessionPtr = std::unique_ptr<BackendSession, SessionDeleter>;

struct FrameBufferDeleter {
    void operator()(IFrameBuffer* buffer) const noexcept;
};
using FrameBufferPtr = std::unique_ptr<IFrameBuffer, FrameBufferDeleter>;

class IEncoderBackend {
public:
    virtual ~IEncoderBackend() = default;

    /// Which backend this is. Not in §7, but the caller needs it to report what opened:
    /// with Encoder::Auto that is the only way to find out.
    virtual Encoder kind() const = 0;

    /// What this backend can do for a given output codec.
    ///
    /// §7 spells this without an argument. It takes one here because the answer genuinely
    /// differs: on Alder Lake HEVC encodes through EncSliceLP with CQP only, and H.264 does
    /// not share those limits. One capability set for both codecs would have to be the
    /// intersection, which would understate what the hardware does.
    virtual EncoderCapabilities GetCapabilities(Codec codec) const = 0;

    /// Opens an encoder and returns it. StatusCode::DeviceUnavailable means "not on this
    /// machine" -- the caller moves to the next backend in the chain, so an absent GPU must
    /// not come back as an error.
    ///
    /// The whole open sequence lives here rather than in the caller. It used to be split:
    /// the caller found the AVCodec, allocated the context and opened it, and the backend
    /// only filled in the middle. That split was three libavcodec calls sitting in code
    /// that is supposed to know nothing about libavcodec, and a backend talking to an SDK
    /// directly could not have used any of them.
    virtual Result<SessionPtr> Configure(const EncoderConfig& config) const = 0;

    /// A frame allocated where the encoder can read it without a copy. The caller fills
    /// plane(), calls commit(), and submits it.
    ///
    /// StatusCode::Unsupported is a legitimate answer: a backend that cannot hand out its
    /// own memory says so, and the caller falls back to submitting planes as usual.
    virtual Result<FrameBufferPtr> CreateFrame(
        BackendSession& session, MemoryType memory) const = 0;

    /// Hands one frame to the encoder.
    ///
    /// A reference, not a pointer, and a buffer rather than a struct with three optional
    /// shapes in it. By the time a frame reaches a backend there is exactly one shape --
    /// pixels somewhere it can read them -- so there is nothing here to be null and
    /// nothing to decide. A caller with planes in system memory wraps them in a
    /// PlaneBuffer, which costs nothing and says so at the call site.
    virtual Status SubmitFrame(BackendSession& session, IFrameBuffer& frame,
        const BackendFrameControl& control) const = 0;

    /// Takes one packet if there is one. StatusCode::Again means "feed more frames",
    /// EndOfStream means there will be no more -- neither is a failure.
    virtual Result<EncodedPacket> ReceivePacket(BackendSession& session) const = 0;

    /// Tells the encoder no more frames are coming. Packets still come out through
    /// ReceivePacket afterwards.
    virtual Status Flush(BackendSession& session) const = 0;

    /// Some backends open successfully and only fail on the first frame. Those ask for a
    /// probe: the caller encodes one throwaway frame, and reopens if it worked.
    virtual bool NeedsProbe() const { return false; }
};

/// A backend, borrowed. Never null, never owned, and outliving any caller: the backends are
/// immortal singletons, so there is nothing here for a smart pointer to manage -- unique_ptr
/// would claim an ownership nobody has, and shared_ptr would count references to objects
/// that are never released.
using BackendRef = std::reference_wrapper<const IEncoderBackend>;

/// The backends to try, in order, for a requested kind. Encoder::Auto gives the whole
/// fallback chain; a named kind gives just that one, so asking for QSV and getting libx264
/// is not something that can happen silently.
///
/// A span over storage that outlives the program, so nothing is allocated to answer and
/// nothing has to be freed:
///
///     for (const IEncoderBackend& b : backend_chain(Encoder::Auto)) { ... }
std::span<const BackendRef> backend_chain(Encoder requested);

} // namespace sve
