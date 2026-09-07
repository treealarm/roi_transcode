#include "backend/common/av_pixel_format.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace sve {

int to_av_pix_fmt(PixelFormat f)
{
    switch (f) {
    case PixelFormat::YUV420P: return AV_PIX_FMT_YUV420P;
    case PixelFormat::NV12:    return AV_PIX_FMT_NV12;
    case PixelFormat::YUV422P: return AV_PIX_FMT_YUV422P;
    case PixelFormat::YUV444P: return AV_PIX_FMT_YUV444P;
    case PixelFormat::GRAY8:   return AV_PIX_FMT_GRAY8;
    case PixelFormat::P010:    return AV_PIX_FMT_P010;
    // Named in the host's byte order, like the enumerator itself: AV_PIX_FMT_YUV420P10 is
    // an alias for whichever of the two endian spellings this machine uses.
    case PixelFormat::YUV420P10: return AV_PIX_FMT_YUV420P10;
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
    case AV_PIX_FMT_P010:    return PixelFormat::P010;
    case AV_PIX_FMT_YUV420P10: return PixelFormat::YUV420P10;
    // A frame the CPU cannot address: QSV, VAAPI, CUDA and the rest. They share one
    // answer here because what a caller can do with them is the same -- nothing directly.
    case AV_PIX_FMT_QSV:
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_DRM_PRIME: return PixelFormat::HardwareSurface;
    default:                   return PixelFormat::Unknown;
    }
}

} // namespace sve
