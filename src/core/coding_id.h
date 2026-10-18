#pragma once

// The output codec, in libavcodec's vocabulary and in ours.
//
// Internal, and small on purpose. It exists because the same two-line ternary had appeared
// in three places -- the backend opening an encoder, the backend labelling a packet, and
// the file path resolving Coding::SameAsSource -- and a fourth copy is how the day comes
// when one of them learns about AV1 and the others do not.

#include "ta_roienc/basics.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace ta_roienc {

/// Anything that is not HEVC becomes H.264. That is the SameAsSource rule (basics.h) and it
/// is also right for an encoder context, where the id can only be one of the two.
constexpr Coding codec_from_av_id(AVCodecID id)
{
    return id == AV_CODEC_ID_HEVC ? Coding::H265 : Coding::H264;
}

/// Coding::SameAsSource never reaches here: it is resolved before any backend is asked for
/// anything, so treating it as HEVC would be a silent wrong answer rather than a default.
constexpr AVCodecID av_id_from_codec(Coding codec)
{
    return codec == Coding::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

} // namespace ta_roienc
