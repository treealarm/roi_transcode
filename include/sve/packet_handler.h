#pragma once

// Where encoded packets go when the caller wants them rather than a file.
//
// Deliberately free of FFmpeg types: a consumer gets bytes and timestamps, and
// does not need libav* on its include path to write a handler. Muxing to a file
// is not done this way -- set SessionParams::output_path and the session does it.
//
// The packet itself lives in packet.h (TZ v3.4 §8.6); this header is the sink it
// arrives at.

#include "sve/packet.h"
#include "sve/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sve {

// Reported once, before any packet.
struct StreamInfo {
    int32_t width = 0;
    int32_t height = 0;
    // Codec configuration for the output. A remote consumer needs it to decode
    // what follows.
    // Borrowed for the length of the on_open call, and carrying its own length.
    std::span<const uint8_t> extradata;
    // Which backend actually opened; with Backend::Auto this is the only way
    // to find out.
    Backend encoder_used = Backend::X264;
    std::string backend_name;
};

class PacketHandler {
public:
    virtual ~PacketHandler() = default;
    virtual void on_open(const StreamInfo&) {}
    virtual void on_packet(const EncodedPacket&) = 0;
    virtual void on_end() {}
};

} // namespace sve
