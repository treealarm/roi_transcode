#pragma once

// What a decoder needs before the first packet -- TZ v3.4 §8.11.
//
// Asked for separately from the packet stream, because the two have different lifetimes: a
// muxer needs this once to declare its stream, while packets arrive for as long as the
// encode runs. It arrived here in the slice that built the encoder rather than the one that
// nominally owns §8.11, because the file path needed it the moment the muxer stopped being
// handed an AVCodecContext -- and handing one over is the FFmpeg leak the public headers
// exist to avoid.

#include "sve/packet.h"
#include "sve/types.h"

#include <cstdint>

namespace sve {

struct CodecParameters {
    Codec codec = Codec::H265;
    /// Backend-specific codec configuration -- SPS/PPS/VPS for the codecs here. Empty until
    /// the encoder has opened, and stable afterwards unless a reconfigure replaces it.
    PacketBuffer extradata;
    /// Units of every timestamp on the packets that follow.
    Rational time_base{1, 90000};
    int32_t width = 0;
    int32_t height = 0;
};

} // namespace sve
