#pragma once

// Frames in, packets out through a callback -- TZ v3.4 §8.8 and §8.9.
//
// The same encoder as encoder.h, turned around. There the caller pulls; here the caller
// hands over a frame and is called back with whatever came out:
//
//     auto push = PushVideoEncoder::Create(config);
//     push.value()->Subscribe([](EncoderEvent e) { ... });
//     push.value()->Process(TimedFrame{...});
//     push.value()->Flush();     // and the last packets arrive in the callback
//
// A packet is not the frame that was just handed over. Encoders buffer -- B-frames,
// lookahead -- so packets come out later and in decode order, and the ones still held when
// the input ends come out on Flush. That is what makes this API asynchronous, and it is the
// reason a return value could not carry the answer.
//
// **The callback's thread is not promised.** Today it runs on the thread that called
// Process, and an implementation with a worker of its own would run it elsewhere. Write the
// callback as if the second case were true: guard what it touches, and do not assume it is
// yours. Saying this now costs nothing; saying it later would silently break every caller
// who had assumed otherwise.
//
// The callback must not throw. An exception crossing it would unwind through code holding a
// frame in flight; report failures by returning, and read them from the Error event.

#include "sve/encoder.h"
#include "sve/timed_frame.h"
#include "sve/input_format.h"
#include "sve/status.h"

#include <functional>
#include <memory>

namespace sve {

enum class EncoderEventType {
    /// `packet` is set, `status` is Ok.
    Packet,
    /// No more packets will come. `packet` is empty.
    EndOfStream,
    /// `packet` is empty and `status` carries the reason. Whether the encoder can go on
    /// depends on the state it is left in -- a fatal one moves it to Failed.
    Error,
};

const char* encoder_event_type_name(EncoderEventType type);

/// Typed, because `nullptr` as a signal for end-of-stream is a contract nobody can read at
/// the call site and everybody has to remember (§8.9).
struct EncoderEvent {
    EncoderEventType type = EncoderEventType::Packet;
    std::shared_ptr<const EncodedPacket> packet;
    Status status;
};

using EncoderCallback = std::function<void(EncoderEvent)>;

class PushVideoEncoder;

/// The push facade's factory -- TZ v4.1 §8.12. Same shape and the same reasons as
/// CreateVideoEncoder, and the two share one implementation of validation and selection.
///
/// Validates the configuration. The backend is not opened here: the input format may still
/// arrive through SetInputFormat, and until it is known there is nothing to open. A backend
/// that will not open is therefore reported from the first Process, as an Error event --
/// not thrown, because by then the caller has a callback and events are how this facade
/// speaks. Throws EncoderError only for what can be judged now, at creation.
std::unique_ptr<PushVideoEncoder> CreatePushVideoEncoder(const SmartEncoderConfig& config);

class PushVideoEncoder {
public:
    ~PushVideoEncoder();
    PushVideoEncoder(const PushVideoEncoder&) = delete;
    PushVideoEncoder& operator=(const PushVideoEncoder&) = delete;

    /// Only before the first frame. §8.3 forbids changing the input format once processing
    /// has begun -- that is a different encode, not a reconfiguration of this one -- so
    /// afterwards this returns InvalidArgument.
    Status SetInputFormat(const InputVideoFormat& format);

    /// Where events go. May be replaced; the new one takes effect from the next event.
    Status Subscribe(EncoderCallback callback);

    /// Hands over one frame and delivers whatever the encoder had ready. Opens the backend
    /// on the first call.
    Status Process(TimedFrame frame);

    /// No more frames. Delivers what the codec still holds, then one EndOfStream event.
    Status Flush();

    /// Releases the backend. Idempotent, and safe from any state.
    Status Stop();

    EncoderState State() const;
    EncoderStatistics GetStatistics() const;

private:
    friend std::unique_ptr<PushVideoEncoder> CreatePushVideoEncoder(const SmartEncoderConfig&);

    struct Impl;
    explicit PushVideoEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sve
