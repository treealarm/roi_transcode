#pragma once

// The encoder with region-of-interest side data, independent of where the frames came
// from. A file pipeline and a caller pushing frames want exactly this and nothing else
// around it.
//
// What lives here is backend selection: walking the fallback chain, honouring a probe, and
// reporting which one opened. The frames themselves go through IEncoderBackend (§7) --
// this class no longer touches libavcodec's send/receive pair itself.

#include "backend/common/backend_session.h"
#include "smartvideo/encoder_backend.h"
#include "smartvideo/types.h"

#include <string>
#include <vector>

namespace smartvideo {

class PacketSink;

class RoiEncoder {
public:
    // Opens a backend, falling back down the chain when kind is Auto, then
    // calls sink.begin(). Throws if nothing opens.
    void open(const EncoderConfig& config, PacketSink& sink);

    // The same in two steps, for a caller that must know which backend opened
    // before the sink is told about it.
    void open_deferred(const EncoderConfig& config);
    void attach(PacketSink& sink);

    Encoder kind() const { return m_kind; }
    /// What the backend that actually opened is able to do. Only meaningful after open;
    /// before that there is no backend to ask.
    EncoderCapabilities capabilities() const;
    // Human-readable name of the backend that actually opened, codec included.
    const char* label() const;
    AVCodecContext* ctx() { return session().enc.p; }
    AVRational time_base() const { return session().enc.p->time_base; }

    /// How many frames needed a pixel-format conversion or an upload to the GPU before the
    /// encoder would take them. Both zero is what zero-copy means, and the only way to
    /// tell a factory frame that avoided the copy from one that merely moved it.
    long frames_converted() const { return session().frames_converted; }
    long frames_uploaded() const { return session().frames_uploaded; }

    /// A frame in memory the encoder reads directly, when the backend can hand one out.
    Result<FrameBufferPtr> create_frame(MemoryType memory);

    // `pts` is already in the encoder time base. `rects` are in pixels, strongest first --
    // on overlap the lowest index wins.
    //
    // Pixels in a form the encoder does not read directly are converted, and uploaded if
    // it wants hardware frames; both are counted. A buffer from create_frame needs neither.
    void encode_buffer(IFrameBuffer& pixels, const std::vector<Rect>& rects, int64_t pts);

    // The same for a frame the library decoded itself.
    void encode(AVFrame* frame, const std::vector<Rect>& rects, int64_t pts);
    // Drains the encoder. Does not end the sink.
    void flush();

private:
    // Pulls packets until the backend says there are none, handing each to the sink. The
    // pull loop is here rather than in the backend because §7 puts ReceivePacket on the
    // backend and leaves the loop to whoever owns the pipeline -- which, for the file and
    // frame entry points, is this class.
    void drain();

    // These four backends are all libavcodec, and the file pipeline still needs the codec
    // context for the muxer's stream parameters. A backend on its own SDK would not go
    // through RoiEncoder at all in its present shape -- which is the next thing to fix,
    // not something to pretend is already done.
    LibavSession& session() const { return libav(*m_session.get()); }

    Encoder m_kind = Encoder::Cpu;
    Codec m_codec = Codec::H265;
    const IEncoderBackend* m_backend = nullptr;
    SessionPtr m_session;

    // One frame's worth of the encoder time base, used when the encoder leaves
    // a packet's duration unset.
    int64_t m_frame_duration = 0;
    PacketSink* m_sink = nullptr;
};

} // namespace smartvideo
