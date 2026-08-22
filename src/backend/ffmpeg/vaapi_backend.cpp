// VA-API -- the same Intel iGPU as oneVPL, reached the other way.
//
// Kept as a fallback rather than a preference: it opens on drivers where the oneVPL
// dispatcher finds no runtime implementation, which is a real and silent failure mode
// (see the runtime image's libmfx-gen dependency).

#include "backend/common/backend_common.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace smartvideo {

namespace {

bool init_vaapi_frames(roiav::Buffer& hw, roiav::Buffer& frames, int w, int h)
{
    av_buffer_unref(&frames.p);
    frames.p = av_hwframe_ctx_alloc(hw.p);
    if (!frames.p) {
        return false;
    }
    auto* ctx = reinterpret_cast<AVHWFramesContext*>(frames.p->data);
    ctx->format = AV_PIX_FMT_VAAPI;
    ctx->sw_format = AV_PIX_FMT_NV12;
    ctx->width = w;
    ctx->height = h;
    ctx->initial_pool_size = 20;
    if (av_hwframe_ctx_init(frames.p) < 0) {
        av_buffer_unref(&frames.p);
        return false;
    }
    return true;
}

class VaapiBackend final : public IEncoderBackend {
public:
    Encoder kind() const override { return Encoder::Vaapi; }

    EncoderCapabilities GetCapabilities(Codec) const override
    {
        EncoderCapabilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_rectangles = true;
        c.runtime_roi_update = true;
        // Deliberately conservative. The VA-API limit is the driver's, not the API's, and
        // there is no query for it that works across drivers -- so this is the number the
        // caller truncates to rather than a number iHD promised.
        c.max_roi_regions = 32;
        return c;
    }

    bool Configure(BackendSession& out, const EncoderParams& p) const override
    {
        backend_common::apply_shared(out.enc.p, p);

        if (!backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128")
            && !backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr)) {
            return false;
        }
        if (!init_vaapi_frames(out.hw_device, out.hw_frames, p.width, p.height)) {
            return false;
        }

        out.enc.p->pix_fmt = AV_PIX_FMT_VAAPI;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        av_opt_set(out.enc.p->priv_data, "rc_mode", "CQP", 0);
        return true;
    }
};

} // namespace

const IEncoderBackend& vaapi_backend()
{
    static const VaapiBackend instance;
    return instance;
}

} // namespace smartvideo
