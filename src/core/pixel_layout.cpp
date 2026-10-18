// The spelling of a PixelLayout. Core, and free of FFmpeg on purpose: this is public API
// (declared in ta_roienc/picture_store.h), and a name is not a mapping. The AVPixelFormat
// conversion lives in engine/common/av_pixel_layout.cpp, on the side that links FFmpeg.

#include "ta_roienc/picture_store.h"

namespace ta_roienc {

const char* pixel_layout_name(PixelLayout f)
{
    switch (f) {
    case PixelLayout::YUV420P:         return "YUV420P";
    case PixelLayout::NV12:            return "NV12";
    case PixelLayout::YUV422P:         return "YUV422P";
    case PixelLayout::YUV444P:         return "YUV444P";
    case PixelLayout::GRAY8:           return "GRAY8";
    case PixelLayout::P010:            return "P010";
    case PixelLayout::YUV420P10:       return "YUV420P10";
    case PixelLayout::HardwareSurface: return "HardwareSurface";
    case PixelLayout::Unknown:         break;
    }
    return "Unknown";
}

} // namespace ta_roienc
