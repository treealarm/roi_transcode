// The spelling of a CodingProfile. Public API, declared in ta_roienc/basics.h, and a name only:
// which profile an input's bit depth admits is decided in the encoder, not here.

#include "ta_roienc/basics.h"

namespace ta_roienc {

const char* coding_profile_name(CodingProfile p)
{
    switch (p) {
    case CodingProfile::Auto:         return "auto";
    case CodingProfile::H264Baseline: return "h264-baseline";
    case CodingProfile::H264Main:     return "h264-main";
    case CodingProfile::H264High:     return "h264-high";
    case CodingProfile::H265Main:     return "h265-main";
    case CodingProfile::H265Main10:   return "h265-main10";
    }
    return "?";
}

} // namespace ta_roienc
