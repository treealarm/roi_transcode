#pragma once

// Frames in, encoded packets out, with the quality spent where a detector said it matters.
//
// This is the core public API; Session and transcode_file are built on top of it.
//
// The shape is submit/receive rather than a callback, and both halves are non-blocking:
//
//     VideoEncoder* enc = ...;
//     enc->SubmitFrame(frame, metadata);      // Again when the input queue is full
//     while (enc->ReceivePacket(&packet)) {   // Again when nothing is ready yet
//         ...
//     }
//     enc->Flush();                           // then drain to EndOfStream
//
// One submitted frame produces nought to many packets: an encoder is free to buffer for
// lookahead or B-frames and to emit in decode order later. Nothing here assumes a packet
// belongs to the frame that was just submitted.
//
// **Threads.** One encoder is driven by one thread at a time. There is no lock inside it:
// the state behind submit and receive is plain, and two threads in it at once is a data
// race rather than a slow path.
//
// Separate encoders on separate threads are supported and are the intended way to use this
// library concurrently -- a service transcoding several clips at once gives each its own.
// They share the backends, which is safe because a backend owns nothing: it is a stateless
// immortal singleton and every encode's state lives in its own BackendSession. That claim
// is checked rather than asserted -- tests/unit/concurrency_test.cpp runs eight encoders
// released at the same instant and requires byte-identical output from each.

#include "ta_roienc/backend_selection.h"
#include "ta_roienc/capabilities.h"
#include "ta_roienc/codec_parameters.h"
#include "ta_roienc/logging.h"
#include "ta_roienc/encoder_backend.h"
#include "ta_roienc/frame_buffer.h"
#include "ta_roienc/frame_metadata.h"
#include "ta_roienc/packet.h"
#include "ta_roienc/report.h"
#include "ta_roienc/status.h"
#include "ta_roienc/timed_frame.h"
#include "ta_roienc/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ta_roienc {

/// Where the encoder is in its life.
///
///     Created -> Configured -> Running -> Draining -> EndOfStream -> Stopped
///                                 |                        |
///                                 +--------> Failed <------+
///
/// Explicit because the alternative is each method inventing its own idea of "too late",
/// and a caller discovering the order by trying things.
enum class EncoderState {
    Created,
    Configured,
    Running,
    Draining,
    EndOfStream,
    Stopped,
    Failed,
};

const char* encoder_state_name(EncoderState s);

/// What may change while frames are flowing.
///
/// Everything absent from this structure is fixed at Create. Codec, resolution, backend and
/// pixel format are not runtime knobs: changing any of them is a different encode with a
/// different codec configuration, and pretending otherwise would produce a stream no
/// decoder can follow across the seam.
struct RuntimeEncoderSettings {
    std::optional<uint64_t> target_bitrate;
    std::optional<Rational> frame_rate;
    std::optional<Quality> quality;
};

/// What the encoder has done so far.
///
/// Everything here is a count of something that happened, not a setting echoed back. The
/// two rules it is built to serve: no silent fallback, so every degradation is counted;
/// and "zero-copy" is a claim nobody can check unless the copies are reported.
struct EncoderStatistics {
    int64_t frames_submitted = 0;
    int64_t packets_received = 0;
    int64_t bytes_encoded = 0;

    /// Regions the backend's own limit could not take. Non-zero is not a fault -- it is a
    /// degradation, and it should be visible rather than inferred.
    int64_t regions_dropped = 0;

    /// Frames that needed a pixel-format conversion, and frames that needed an upload,
    /// before the encoder would take them. Both zero is what zero-copy means.
    int64_t frames_converted = 0;
    int64_t frames_uploaded = 0;

    /// Keyframe requests the encoder was asked for. Best effort, so this counts what was
    /// asked rather than what the bitstream ended up containing.
    int64_t keyframes_requested = 0;

    /// Frames whose per-block quality map asked for a quantiser the codec does not have.
    ///
    /// Non-zero means part of the request did nothing: a delta added to the frame's own
    /// quantiser fell outside 1..51 and was capped. Worth reading before concluding that a
    /// larger background penalty had no effect -- past the end of the scale it cannot.
    int64_t frames_quantiser_clamped = 0;

    /// Set when target_bitrate was refused on CPU H.265 and CRF/CQP used instead. A guard
    /// that only appeared in a log would be invisible to anything but a human reading it.
    bool cpu_h265_target_guard = false;

    /// Set when the outer bitrate loop ran out of authority: it has been holding its whole
    /// range -- background crushed, low-importance regions softened -- and the stream is
    /// still above the target band.
    ///
    /// It happens when the expensive part of the frame is inside a protected region: on a
    /// crowd scene whose protected box covers 38% of the frame and all of its detail, moving
    /// the background penalty from +6 to +30 changes the rate by four percent, so no setting
    /// of this loop can reach the target. A caller who asked for a bitrate and is missing it
    /// deserves to know it was arithmetic and not a knob left unturned.
    bool bitrate_target_unreachable = false;

    /// Backends that were tried and declined before one opened. Non-zero is normal on a
    /// machine without the hardware; it is here so "which one opened, and what did it beat"
    /// can be answered without reading a log.
    int64_t backends_declined = 0;

    /// Distinct degradations applied under FallbackPolicy::Permissive: regions cut to the
    /// hardware's limit, a quality map quantized to rectangles, metadata the encoder cannot
    /// carry, a rate-control mode refused. Each is counted once, not per frame.
    int64_t fallbacks_applied = 0;

    /// Which backend actually opened. With Backend::Auto this is the only way to find out.
    Backend encoder_used = Backend::X264;
    std::string backend_name;

    /// ROI lag, when the boxes carried track ids to measure it from.
    bool roi_lag_measured = false;
    int64_t roi_lag_samples = 0;
    float roi_lag_min_iou = 0.0f;
    double roi_lag_p95_frames = 0.0;
    double roi_lag_p99_frames = 0.0;
    double roi_lag_p95_ms = 0.0;
    double roi_lag_p99_ms = 0.0;
    bool roi_lag_acceptance = false;
};

/// Everything the encoder needs, from the input format to how quality is spent.
///
/// `settings` is the adaptive half: which codec and backend, the constant quantizer, and
/// the whole Quality model. `input` is the geometry and timing the backends take.
struct EncoderConfig {
    InputVideoFormat input;
    Settings settings;

    /// Where diagnostics go. Left unset they go to stderr, one line each, which is what a
    /// command-line tool wants.
    LogSink log;
};

class VideoEncoder;

/// Opens a backend and returns an encoder.
///
/// A free function, and the only public way to make one, so that the codec and the backend
/// are never spelled into a factory's name.
///
/// The chain is walked when settings.encoder is Backend::Auto; a named backend is tried
/// alone, so asking for QSV and silently getting libx264 is not something that can happen.
///
/// Throws EncoderError, carrying the StatusCode, when nothing can serve the request rather
/// than returning nullptr: nullptr says only that something went wrong, while the caller's
/// next move depends entirely on which thing -- InvalidArgument is a configuration to fix,
/// DeviceUnavailable is a machine to change.
std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncoderConfig& config);

class VideoEncoder {
public:
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /// Hands one frame over. Non-blocking: `Again` means the input queue is full and the
    /// caller should drain before submitting again. A frame is never accepted in part.
    ///
    /// Takes the frame by value, and the Frame inside is a shared handle, so the encoder
    /// keeping it for lookahead or B-frames costs the caller nothing to allow. An empty
    /// Frame is InvalidArgument.
    ///
    /// There is no AVFrame overload here, deliberately: it is SubmitAvFrame in
    /// `ta_roienc/frame.h`, so that an embedder with their own pixels never sees an FFmpeg type in
    /// the interface they compile against.
    Status SubmitFrame(TimedFrame frame);

    /// The same, for pixels the caller holds and will outlive the call. Borrowed rather
    /// than shared: a buffer the backend handed out through CreateFrame lives in the
    /// encoder's own memory and is reused frame after frame, so wrapping it in a shared
    /// handle every time would count references nobody is racing for. Collapsing the two
    /// overloads would make the zero-copy path pay for ownership it does not need.
    Status SubmitFrame(IFrameBuffer& pixels, const FrameMetadata& metadata);

    /// Takes one packet if there is one. `Again` means feed more frames; `EndOfStream`
    /// means there will be no more. Neither is a failure.
    Status ReceivePacket(EncodedPacket* packet);

    /// No more frames are coming. Moves to Draining; ReceivePacket then yields what is
    /// left and finally EndOfStream.
    Status Flush();

    /// Releases the backend. Idempotent, and safe from any state.
    Status Stop();

    /// Asks for the next accepted frame to be a keyframe. Best effort, counted in
    /// statistics whether or not the encoder obliged.
    Status RequestKeyFrame();

    /// Applies what may change on the fly (RuntimeEncoderSettings). Anything else -- codec,
    /// resolution, backend, pixel format -- returns InvalidArgument rather than reopening
    /// silently.
    Status Reconfigure(const RuntimeEncoderSettings& settings);

    /// What a decoder needs before the first packet. Available as soon as the encoder
    /// exists, so a muxer can declare its stream without waiting for a frame.
    Status GetCodecParameters(CodecParameters* parameters) const;

    EncoderCapabilities GetCapabilities() const;

    /// What the backend that opened actually settled on -- b_frames, lookahead and the two
    /// adaptive-quantisation switches, as they ended up rather than as they were asked for.
    /// A field left unset means the backend cannot report it, which is not the same as it
    /// being off.
    EffectiveSettings GetEffectiveSettings() const;

    /// The hardware device the backend opened, as its own native handle. Empty when the
    /// encoder is running on the CPU.
    ///
    /// Untyped on purpose: naming AVBufferRef here would put an FFmpeg type into this
    /// interface. `ta_roienc/frame.h` -- the one header that may say AVFrame -- turns this into an
    /// AVBufferRef*, and the tag NativeHandle carries is what makes that conversion
    /// checkable rather than assumed.
    ///
    /// There is one caller and it is worth naming: a decoder that hands this encoder frames
    /// without a round trip through system memory has to decode onto the device the encoder
    /// already holds.
    NativeHandle HardwareDevice() const;

    EncoderStatistics GetStatistics() const;
    EncoderState State() const;

    /// The statistics as a Report, for callers that already speak that shape.
    Report ToReport() const;

private:
    friend std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncoderConfig&);
    /// The same assembly against a BackendRegistry the caller built, declared in
    /// src/core/encoder_assembly.h and not here: the registry is chosen by whoever
    /// assembles the deployment, which is this project, not by whoever calls the library.
    /// Named here only because it has to be, to build what CreateVideoEncoder builds.
    /// BackendRegistry is already declared at namespace scope by ta_roienc/types.h.
    friend std::unique_ptr<VideoEncoder> CreateVideoEncoderIn(
        const EncoderConfig&, const BackendRegistry&);

    struct Impl;
    explicit VideoEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace ta_roienc
