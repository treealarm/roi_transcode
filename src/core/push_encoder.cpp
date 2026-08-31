#include "sve/push_encoder.h"

#include <memory>
#include <utility>

namespace sve {

const char* encoder_event_type_name(EncoderEventType type)
{
    switch (type) {
    case EncoderEventType::Packet:      return "packet";
    case EncoderEventType::EndOfStream: return "end-of-stream";
    case EncoderEventType::Error:       return "error";
    }
    return "?";
}

/// Everything here is bookkeeping around SmartVideoEncoder, which is the point: §8.8 asks
/// the facade to carry no encoder logic of its own, and the moment it grows some, the two
/// ways in stop meaning the same thing.
struct PushVideoEncoder::Impl {
    explicit Impl(const EncoderConfig& cfg) : config(cfg) {}

    EncoderConfig config;
    EncoderCallback callback;
    std::unique_ptr<SmartVideoEncoder> encoder;
    bool started = false;
    bool ended = false;

    void emit(EncoderEvent event) const
    {
        if (callback) {
            callback(std::move(event));
        }
    }

    void emit_error(Status status) const
    {
        EncoderEvent event;
        event.type = EncoderEventType::Error;
        event.status = std::move(status);
        emit(std::move(event));
    }

    /// Opens on first use. Failing here is an Error event rather than a thrown anything:
    /// the caller is inside Process and has a frame in hand.
    Status ensure_open()
    {
        if (encoder) {
            return Status::Ok();
        }
        try {
            encoder = CreateVideoEncoder(config);
        } catch (const EncoderError& e) {
            // The one place the facade turns a thrown answer back into a returned one, and
            // the reason is the caller's position: they are inside Process holding a frame,
            // with a callback already subscribed, and events are how this facade speaks.
            // The Status travels whole, so nothing about the failure is lost in the change.
            return e.status();
        }
        return Status::Ok();
    }

    /// Takes everything the encoder has ready and hands each packet over. Again means "no
    /// more for now" and EndOfStream means "no more at all" -- neither is a failure, and
    /// only the second produces an event.
    Status drain()
    {
        for (;;) {
            auto packet = std::make_shared<EncodedPacket>();
            const Status s = encoder->ReceivePacket(packet.get());
            if (s.code() == StatusCode::Again) {
                return Status::Ok();
            }
            if (s.code() == StatusCode::EndOfStream) {
                if (!ended) {
                    ended = true;
                    EncoderEvent event;
                    event.type = EncoderEventType::EndOfStream;
                    event.status = Status::EndOfStream();
                    emit(std::move(event));
                }
                return Status::Ok();
            }
            if (!s.ok()) {
                return s;
            }
            EncoderEvent event;
            event.type = EncoderEventType::Packet;
            event.packet = std::move(packet);
            emit(std::move(event));
        }
    }
};

PushVideoEncoder::PushVideoEncoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PushVideoEncoder::~PushVideoEncoder() = default;

std::unique_ptr<PushVideoEncoder> CreatePushVideoEncoder(const EncoderConfig& config)
{
    return std::unique_ptr<PushVideoEncoder>(
        new PushVideoEncoder(std::make_unique<PushVideoEncoder::Impl>(config)));
}

Status PushVideoEncoder::SetInputFormat(const InputVideoFormat& format)
{
    if (m_impl->started) {
        return Status::InvalidArgument(
            "the input format cannot change once frames have been processed");
    }
    if (!format.valid()) {
        return Status::InvalidArgument("input video format is not usable");
    }
    m_impl->config.input = format;
    return Status::Ok();
}

Status PushVideoEncoder::Subscribe(EncoderCallback callback)
{
    m_impl->callback = std::move(callback);
    return Status::Ok();
}

Status PushVideoEncoder::Process(TimedFrame frame)
{
    Impl& s = *m_impl;
    if (!frame.frame) {
        return Status::InvalidArgument("Process needs a frame");
    }
    if (const Status opened = s.ensure_open(); !opened.ok()) {
        s.emit_error(opened);
        return opened;
    }
    s.started = true;

    const Status submitted = s.encoder->SubmitFrame(std::move(frame));
    if (!submitted.ok() && submitted.code() != StatusCode::Again) {
        s.emit_error(submitted);
        return submitted;
    }
    if (const Status drained = s.drain(); !drained.ok()) {
        s.emit_error(drained);
        return drained;
    }
    // Again survives to the caller: §24 puts backpressure in a code, not in a block, and
    // the frame was not taken. Whatever was ready has been delivered by now, so retrying
    // has somewhere to go.
    return submitted;
}

Status PushVideoEncoder::Flush()
{
    Impl& s = *m_impl;
    if (!s.encoder) {
        // Nothing was ever encoded. The stream still ends, because a caller that flushes
        // an empty encoder is entitled to the same event as one that flushes a full one.
        if (!s.ended) {
            s.ended = true;
            EncoderEvent event;
            event.type = EncoderEventType::EndOfStream;
            event.status = Status::EndOfStream();
            s.emit(std::move(event));
        }
        return Status::Ok();
    }
    if (const Status flushed = s.encoder->Flush(); !flushed.ok()) {
        s.emit_error(flushed);
        return flushed;
    }
    if (const Status drained = s.drain(); !drained.ok()) {
        s.emit_error(drained);
        return drained;
    }
    return Status::Ok();
}

Status PushVideoEncoder::Stop()
{
    if (m_impl->encoder) {
        return m_impl->encoder->Stop();
    }
    return Status::Ok();
}

EncoderState PushVideoEncoder::State() const
{
    return m_impl->encoder ? m_impl->encoder->State() : EncoderState::Created;
}

EncoderStatistics PushVideoEncoder::GetStatistics() const
{
    return m_impl->encoder ? m_impl->encoder->GetStatistics() : EncoderStatistics{};
}

} // namespace sve
