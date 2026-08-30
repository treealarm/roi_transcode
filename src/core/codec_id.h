#pragma once

// The output codec, in libavcodec's vocabulary and in ours.
//
// Internal, and small on purpose. It exists because the same two-line ternary had appeared
// in three places -- the backend opening an encoder, the backend labelling a packet, and
// the file path resolving Codec::SameAsSource -- and a fourth copy is how the day comes
// when one of them learns about AV1 and the others do not.

#include "sve/types.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace sve {

/// Anything that is not HEVC becomes H.264. That is the SameAsSource rule (types.h) and it
/// is also right for an encoder context, where the id can only be one of the two.
constexpr Codec codec_from_av_id(AVCodecID id)
{
    return id == AV_CODEC_ID_HEVC ? Codec::H265 : Codec::H264;
}

/// Codec::SameAsSource never reaches here: it is resolved before any backend is asked for
/// anything, so treating it as HEVC would be a silent wrong answer rather than a default.
constexpr AVCodecID av_id_from_codec(Codec codec)
{
    return codec == Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

} // namespace sve
