#pragma once

// Frames in, encoded packets out, with the quality spent where a detector said it matters.
// TZ v3.4 §8.1, §8.13 and §24.
//
// This is the normative public API. The two older ways in -- Session and transcode_file --
// are being rewritten on top of it; transcode_file already is.
//
// The shape is submit/receive rather than a callback, and both halves are non-blocking:
//
//     SmartVideoEncoder* enc = ...;
//     enc->SubmitFrame(frame, metadata);      // Again when the input queue is full
//     while (enc->ReceivePacket(&packet)) {   // Again when nothing is ready yet
//         ...
//     }
//     enc->Flush();                           // then drain to EndOfStream
//
// One submitted frame produces nought to many packets: an encoder is free to buffer for
// lookahead or B-frames and to emit in decode order later. Nothing here assumes a packet
// belongs to the frame that was just submitted.

#include "sve/backend_selection.h"
#include "sve/capabilities.h"
#include "sve/codec_parameters.h"
#include "sve/logging.h"
#include "sve/encoder_backend.h"
#include "sve/frame_buffer.h"
#include "sve/frame_metadata.h"
#include "sve/packet.h"
#include "sve/report.h"
#include "sve/status.h"
#include "sve/timed_frame.h"
#include "sve/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sve {

/// Where the encoder is in its life -- TZ v3.4 §8.13.
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

/// What may change while frames are flowing -- TZ v3.4 §8.2.
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

/// What the encoder has done so far -- TZ v3.4 §8.1, and Appendix C leaves the shape to us.
///
/// Everything here is a count of something that happened, not a setting echoed back. The
/// two rules it is built to serve: §50 forbids a silent fallback, so every degradation is
/// counted; and "zero-copy" is a claim nobody can check unless the copies are reported.
struct EncoderStatistics {
    int64_t frames_submitted = 0;
    int64_t packets_received = 0;
    int64_t bytes_encoded = 0;

    /// Regions the backend's own limit could not take. Non-zero is not a fault -- it is a
    /// degradation, and §50 wants it visible rather than inferred.
    int64_t regions_dropped = 0;

    /// Frames that needed a pixel-format conversion, and frames that needed an upload,
    /// before the encoder would take them. Both zero is what zero-copy means.
    int64_t frames_converted = 0;
    int64_t frames_uploaded = 0;

    /// Keyframe requests the encoder was asked for. Best effort by §8.1, so this counts
    /// what was asked rather than what the bitstream ended up containing.
    int64_t keyframes_requested = 0;

    /// Set when target_bitrate was refused on CPU H.265 and CRF/CQP used instead. A guard
    /// that only appeared in a log would be invisible to anything but a human reading it.
    bool cpu_h265_target_guard = false;

    /// Backends that were tried and declined before one opened. Non-zero is normal on a
    /// machine without the hardware; it is here so "which one opened, and what did it beat"
    /// can be answered without reading a log (§22).
    int64_t backends_declined = 0;

    /// Distinct degradations applied under FallbackPolicy::Permissive: regions cut to the
    /// hardware's limit, a quality map quantized to rectangles, metadata the encoder cannot
    /// carry, a rate-control mode refused. Each is counted once, not per frame.
    int64_t fallbacks_applied = 0;

    /// Which backend actually opened. With Backend::Auto this is the only way to find out.
    Backend encoder_used = Backend::X264;
    std::string backend_name;

    /// §14 ROI lag, when the boxes carried track ids to measure it from.
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
/// `settings` is the adaptive half (TZ calls it AdaptiveConfig; Appendix C leaves it open):
/// which codec and backend, the constant quantizer, and the whole Quality model. `backend`
/// is the geometry and timing the backends take.
struct EncoderConfig {
    InputVideoFormat input;
    Settings settings;

    /// Which backends Backend::Auto may consider, and in what order (§8.4). Ignored when
    /// settings.encoder names one.
    BackendSelection backend_selection;

    /// Where diagnostics go (§29). Left unset they go to stderr, one line each, which is
    /// what a command-line tool and the acceptance scripts want.
    LogSink log;
};

class SmartVideoEncoder;

/// Opens a backend and returns an encoder -- TZ v4.1 §8.12.
///
/// A free function, and the only public way to make one. §8.12 asks that the codec and the
/// backend not be spelled into a factory's name, and a static Create on the class is that
/// same mistake one level up: it makes the class the thing you name to get an encoder,
/// which is what a second entry point would then have to be named against.
///
/// The chain is walked when settings.encoder is Backend::Auto; a named backend is tried
/// alone, so asking for QSV and silently getting libx264 is not something that can happen.
///
/// Throws EncoderError, carrying the StatusCode, when nothing can serve the request.
/// Returning nullptr instead is forbidden by §8.12, and rightly: nullptr says only that
/// something went wrong, while the caller's next move depends entirely on which thing --
/// InvalidArgument is a configuration to fix, DeviceUnavailable is a machine to change.
std::unique_ptr<SmartVideoEncoder> CreateVideoEncoder(const EncoderConfig& config);

class SmartVideoEncoder {
public:
    ~SmartVideoEncoder();
    SmartVideoEncoder(const SmartVideoEncoder&) = delete;
    SmartVideoEncoder& operator=(const SmartVideoEncoder&) = delete;

    /// Hands one frame over -- TZ v4.1 §8.1. Non-blocking: `Again` means the input queue
    /// is full and the caller should drain before submitting again. A frame is never
    /// accepted in part.
    ///
    /// Takes the frame by value, and the Frame inside is a shared handle, so the encoder
    /// keeping it for lookahead or B-frames costs the caller nothing to allow. An empty
    /// Frame is InvalidArgument.
    ///
    /// There is no AVFrame overload here, deliberately: §8.1 puts it in `sve/frame.h` as
    /// SubmitAvFrame, so that an embedder with their own pixels never sees an FFmpeg type
    /// in the interface they compile against.
    Status SubmitFrame(TimedFrame frame);

    /// The same, for pixels the caller holds and will outlive the call. Borrowed rather
    /// than shared: a buffer the backend handed out through CreateFrame lives in the
    /// encoder's own memory and is reused frame after frame, so wrapping it in a shared
    /// handle every time would count references nobody is racing for.
    ///
    /// A departure from §8.1, which spells one SubmitFrame. Kept because the difference is
    /// real -- one owns, one does not -- and collapsing them would make the zero-copy path
    /// pay for ownership it does not need.
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

    /// Applies what §8.2 allows to change on the fly. Anything else -- codec, resolution,
    /// backend, pixel format -- returns InvalidArgument rather than reopening silently.
    Status Reconfigure(const RuntimeEncoderSettings& settings);

    /// What a decoder needs before the first packet -- TZ v3.4 §8.11. Available as soon as
    /// the encoder exists, so a muxer can declare its stream without waiting for a frame.
    Status GetCodecParameters(CodecParameters* parameters) const;

    EncoderCapabilities GetCapabilities() const;
    EncoderStatistics GetStatistics() const;
    EncoderState State() const;

    /// The statistics as a Report, for callers that already speak that shape.
    Report ToReport() const;

private:
    friend std::unique_ptr<SmartVideoEncoder> CreateVideoEncoder(const EncoderConfig&);

    struct Impl;
    explicit SmartVideoEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sve
