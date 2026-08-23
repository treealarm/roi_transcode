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

class VaapiBackend final : public LibavBackend {
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

    const char* codec_name(Codec codec) const override
    {
        return codec == Codec::H264 ? "h264_vaapi" : "hevc_vaapi";
    }

    Status configure_context(LibavSession& out, const EncoderConfig& p) const override
    {
        if (!backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128")
            && !backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr)) {
            return Status::Unavailable("no VA-API device on this machine");
        }
        if (!backend_common::init_hw_frames(out.hw_device, out.hw_frames,
                AV_PIX_FMT_VAAPI, AV_PIX_FMT_NV12, p.width, p.height, 20)) {
            return Status::Unavailable("VA-API device opened but allocates no frames");
        }

        out.enc.p->pix_fmt = AV_PIX_FMT_VAAPI;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        av_opt_set(out.enc.p->priv_data, "rc_mode", "CQP", 0);
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& vaapi_backend()
{
    static const VaapiBackend instance;
    return instance;
}

} // namespace smartvideo
