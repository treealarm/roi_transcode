#pragma once

// Where encoded packets go when the caller wants them rather than a file.
//
// Deliberately free of FFmpeg types: a consumer gets bytes and timestamps, and
// does not need libav* on its include path to write a handler. Muxing to a file
// is not done this way -- set SessionParams::output_path and the session does it.

#include "sve/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sve {

// One encoded access unit. `data` belongs to whoever handed it over and stays valid until
// the next call on the same session -- copy it if you need it longer.
struct EncodedPacket {
    // Borrowed: valid until the next call on the same session.
    std::span<const uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    bool key = false;
    // Units of pts, dts and duration. The session's time base on the way out to a handler,
    // the encoder's on the way out of a backend -- which are the same thing unless the
    // caller asked for their own.
    Rational time_base{1, 1};
};

// Reported once, before any packet.
struct StreamInfo {
    int32_t width = 0;
    int32_t height = 0;
    // Codec configuration for the output. A remote consumer needs it to decode
    // what follows.
    // Borrowed for the length of the on_open call, and carrying its own length.
    std::span<const uint8_t> extradata;
    // Which backend actually opened; with Encoder::Auto this is the only way
    // to find out.
    Encoder encoder_used = Encoder::Cpu;
    std::string encoder_name;
};

class PacketHandler {
public:
    virtual ~PacketHandler() = default;
    virtual void on_open(const StreamInfo&) {}
    virtual void on_packet(const EncodedPacket&) = 0;
    virtual void on_end() {}
};

} // namespace sve
