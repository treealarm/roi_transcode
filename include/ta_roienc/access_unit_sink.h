#pragma once

// Where encoded packets go when the caller wants them rather than a file.
//
// Deliberately free of FFmpeg types: a consumer gets bytes and timestamps, and
// does not need libav* on its include path to write a handler. Muxing to a file
// is not done this way -- set TranscoderOptions::output_path and the session does it.
//
// The packet itself lives in access_unit.h; this header is the sink it arrives at.

#include "ta_roienc/access_unit.h"
#include "ta_roienc/basics.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ta_roienc {

// Reported once, before any packet.
struct SourceInfo {
    int32_t width = 0;
    int32_t height = 0;
    // Codec configuration for the output. A remote consumer needs it to decode
    // what follows.
    // Borrowed for the length of the on_start call, and carrying its own length.
    std::span<const uint8_t> extradata;
    // Which backend actually opened; with Engine::Auto this is the only way
    // to find out.
    Engine engine_used = Engine::X264;
    std::string engine_name;
};

class AccessUnitSink {
public:
    virtual ~AccessUnitSink() = default;
    virtual void on_start(const SourceInfo&) {}
    virtual void on_access_unit(const AccessUnit&) = 0;
    virtual void on_finish() {}
};

} // namespace ta_roienc
