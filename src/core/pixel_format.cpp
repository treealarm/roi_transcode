// PixelFormat to AVPixelFormat and back.
//
// The mapping lives here and nowhere else. That is the whole point of the library having
// its own enum: FFmpeg numbers its formats positionally in a list it appends to, so a
// public header that borrowed those numbers would be promising something another project's
// release notes get to break. Here it can only be wrong in one place.

#include "smartvideo/frame_buffer.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace smartvideo {

const char* pixel_format_name(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P:         return "YUV420P";
    case PixelFormat::NV12:            return "NV12";
    case PixelFormat::YUV422P:         return "YUV422P";
    case PixelFormat::YUV444P:         return "YUV444P";
    case PixelFormat::GRAY8:           return "GRAY8";
    case PixelFormat::HardwareSurface: return "HardwareSurface";
    case PixelFormat::Unknown:         break;
    }
    return "Unknown";
}

int to_av_pix_fmt(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P: return AV_PIX_FMT_YUV420P;
    case PixelFormat::NV12:    return AV_PIX_FMT_NV12;
    case PixelFormat::YUV422P: return AV_PIX_FMT_YUV422P;
    case PixelFormat::YUV444P: return AV_PIX_FMT_YUV444P;
    case PixelFormat::GRAY8:   return AV_PIX_FMT_GRAY8;
    default:                   return AV_PIX_FMT_NONE;
    }
}

PixelFormat from_av_pix_fmt(int f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
    case AV_PIX_FMT_NV12:    return PixelFormat::NV12;
    case AV_PIX_FMT_YUV422P: return PixelFormat::YUV422P;
    case AV_PIX_FMT_YUV444P: return PixelFormat::YUV444P;
    case AV_PIX_FMT_GRAY8:   return PixelFormat::GRAY8;
    // A frame the CPU cannot address: QSV, VAAPI, CUDA and the rest. They share one
    // answer here because what a caller can do with them is the same -- nothing directly.
    case AV_PIX_FMT_QSV:
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_DRM_PRIME: return PixelFormat::HardwareSurface;
    default:                   return PixelFormat::Unknown;
    }
}

} // namespace smartvideo
