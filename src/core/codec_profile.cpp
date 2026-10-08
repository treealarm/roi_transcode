// The spelling of a CodecProfile. Public API, declared in ta_roienc/types.h, and a name only:
// which profile an input's bit depth admits is decided in the encoder, not here.

#include "ta_roienc/types.h"

namespace ta_roienc {

const char* codec_profile_name(CodecProfile p)
{
    switch (p) {
    case CodecProfile::Auto:         return "auto";
    case CodecProfile::H264Baseline: return "h264-baseline";
    case CodecProfile::H264Main:     return "h264-main";
    case CodecProfile::H264High:     return "h264-high";
    case CodecProfile::H265Main:     return "h265-main";
    case CodecProfile::H265Main10:   return "h265-main10";
    }
    return "?";
}

} // namespace ta_roienc
