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
#include "sve/frame_metadata.h"
#include "sve/packet.h"
#include "sve/report.h"
#include "sve/status.h"
#include "sve/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct AVFrame;

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

    /// Which backend actually opened. With Encoder::Auto this is the only way to find out.
    Encoder encoder_used = Encoder::Cpu;
    std::string encoder_name;

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
struct SmartEncoderConfig {
    InputVideoFormat input;
    Settings settings;

    /// Which backends Encoder::Auto may consider, and in what order (§8.4). Ignored when
    /// settings.encoder names one.
    BackendSelection backend_selection;

    /// Where diagnostics go (§29). Left unset they go to stderr, one line each, which is
    /// what a command-line tool and the acceptance scripts want.
    LogSink log;
};

class SmartVideoEncoder {
public:
    /// Opens a backend and returns an encoder, or the reason none opened. The chain is
    /// walked when settings.encoder is Encoder::Auto; a named backend is tried alone, so
    /// asking for QSV and silently getting libx264 is not something that can happen.
    static Result<std::unique_ptr<SmartVideoEncoder>> Create(const SmartEncoderConfig& config);

    ~SmartVideoEncoder();
    SmartVideoEncoder(const SmartVideoEncoder&) = delete;
    SmartVideoEncoder& operator=(const SmartVideoEncoder&) = delete;

    /// Hands one frame over. Non-blocking: `Again` means the input queue is full and the
    /// caller should drain before submitting again. A frame is never accepted in part.
    ///
    /// The frame is borrowed for the length of the call; the implementation takes its own
    /// reference if it keeps it. `metadata` is borrowed too, spans and all.
    Status SubmitFrame(const AVFrame* frame, const FrameMetadata& metadata);

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
    struct Impl;
    explicit SmartVideoEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sve
