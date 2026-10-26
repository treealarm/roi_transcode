#include "engine/common/av_pixel_layout.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace ta_roienc {

int to_av_pix_fmt(PixelLayout f)
{
    switch (f) {
    case PixelLayout::YUV420P: return AV_PIX_FMT_YUV420P;
    case PixelLayout::NV12:    return AV_PIX_FMT_NV12;
    case PixelLayout::YUV422P: return AV_PIX_FMT_YUV422P;
    case PixelLayout::YUV444P: return AV_PIX_FMT_YUV444P;
    case PixelLayout::GRAY8:   return AV_PIX_FMT_GRAY8;
    case PixelLayout::P010:    return AV_PIX_FMT_P010;
    // Named in the host's byte order, like the enumerator itself: AV_PIX_FMT_YUV420P10 is
    // an alias for whichever of the two endian spellings this machine uses.
    case PixelLayout::YUV420P10: return AV_PIX_FMT_YUV420P10;
    default:                   return AV_PIX_FMT_NONE;
    }
}

PixelLayout from_av_pix_fmt(int f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return PixelLayout::YUV420P;
    case AV_PIX_FMT_NV12:    return PixelLayout::NV12;
    case AV_PIX_FMT_YUV422P: return PixelLayout::YUV422P;
    case AV_PIX_FMT_YUV444P: return PixelLayout::YUV444P;
    case AV_PIX_FMT_GRAY8:   return PixelLayout::GRAY8;
    case AV_PIX_FMT_P010:    return PixelLayout::P010;
    case AV_PIX_FMT_YUV420P10: return PixelLayout::YUV420P10;
    // A frame the CPU cannot address: QSV, VAAPI, CUDA, D3D11 and the rest. They share one
    // answer here because what a caller can do with them is the same -- nothing directly.
    case AV_PIX_FMT_QSV:
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_D3D11:
    case AV_PIX_FMT_DRM_PRIME: return PixelLayout::HardwareSurface;
    default:                   return PixelLayout::Unknown;
    }
}

} // namespace ta_roienc
