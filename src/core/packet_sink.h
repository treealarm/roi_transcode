#pragma once

// Where encoded packets go. The encoder does not know whether it is filling a
// file or handing bytes back to a caller, which is the whole point: the frame
// mode needs the same encoder without a muxer behind it.
//
// This header names no FFmpeg type, and that is load-bearing rather than tidy. It is the
// interface the whole pipeline hangs off, so anything it includes, the core includes.
// FileMuxSink -- the one implementation that does need libavformat -- lives next door in
// file_mux_sink.h (TZ v4.1 §5.5: encoding modules do not depend on FFmpeg, and the muxer
// is an example application's concern, §52).

#include "sve/codec_parameters.h"
#include "sve/packet.h"

namespace sve {

class PacketSink {
public:
    virtual ~PacketSink() = default;

    // Called once, after the encoder is open and before any packet arrives.
    //
    // CodecParameters rather than an AVCodecContext, and that is the change TZ v4.1 §20
    // forces. A sink taking a context could only ever be fed by a backend that had one,
    // which made this interface -- the one the whole file pipeline hangs off -- a
    // libavcodec interface wearing a neutral name. A backend on its own SDK could compile
    // and could not deliver a packet anywhere.
    virtual void begin(const CodecParameters& params) = 0;
    // The packet carries its own time base, so there is nothing to pass beside it and
    // nothing for a caller to get out of step with.
    virtual void on_packet(const EncodedPacket& packet) = 0;
    virtual void end() = 0;
};

} // namespace sve
