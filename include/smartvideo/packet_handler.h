#pragma once

// Where encoded packets go when the caller wants them rather than a file.
//
// Deliberately free of FFmpeg types: a consumer gets bytes and timestamps, and
// does not need libav* on its include path to write a handler. Muxing to a file
// is not done this way -- set SessionParams::output_path and the session does it.

#include "smartvideo/types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace smartvideo {

struct EncodedPacket {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    // In the session's time base.
    int64_t pts = 0;
    int64_t dts = 0;
    bool key = false;
};

// Reported once, before any packet.
struct StreamInfo {
    int width = 0;
    int height = 0;
    // Codec configuration for the output. A remote consumer needs it to decode
    // what follows.
    const uint8_t* extradata = nullptr;
    std::size_t extradata_size = 0;
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

} // namespace smartvideo
