#include "backend/common/backend_common.h"

#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace smartvideo::backend_common {

void apply_shared(AVCodecContext* enc, const EncoderParams& p)
{
    enc->width = p.width;
    enc->height = p.height;
    enc->time_base = p.time_base.den > 0 ? p.time_base : av_inv_q(p.fps);
    enc->framerate = p.fps;
    // Two seconds' worth when nothing was asked for, which is what a player expects to
    // wait before it can start.
    const int gop_auto = std::max(p.fps.num / std::max(p.fps.den, 1), 1) * 2;
    enc->gop_size = p.gop > 0 ? p.gop : gop_auto;
    enc->max_b_frames = 0;
}

bool open_hw_device(roiav::Buffer& hw, AVHWDeviceType type, const char* device)
{
    av_buffer_unref(&hw.p);
    return av_hwdevice_ctx_create(&hw.p, type, device, nullptr, 0) >= 0;
}

} // namespace smartvideo::backend_common
