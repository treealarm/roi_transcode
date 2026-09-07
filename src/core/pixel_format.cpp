// The spelling of a PixelFormat. Core, and free of FFmpeg on purpose: this is public API
// (declared in sve/frame_buffer.h), and a name is not a mapping. The AVPixelFormat
// conversion that used to share this file moved to backend/common/av_pixel_format.cpp
// when §5.5 drew the line between the core and anything that links FFmpeg.

#include "sve/frame_buffer.h"

namespace sve {

const char* pixel_format_name(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P:         return "YUV420P";
    case PixelFormat::NV12:            return "NV12";
    case PixelFormat::YUV422P:         return "YUV422P";
    case PixelFormat::YUV444P:         return "YUV444P";
    case PixelFormat::GRAY8:           return "GRAY8";
    case PixelFormat::P010:            return "P010";
    case PixelFormat::YUV420P10:       return "YUV420P10";
    case PixelFormat::HardwareSurface: return "HardwareSurface";
    case PixelFormat::Unknown:         break;
    }
    return "Unknown";
}

} // namespace sve
