// VA-API -- the same Intel iGPU as oneVPL, reached the other way.
//
// Kept as a fallback rather than a preference: it opens on drivers where the oneVPL
// dispatcher finds no runtime implementation, which is a real and silent failure mode
// (see the runtime image's libmfx-gen dependency).

#include "engine/common/engine_common.h"
#include "engine/common/intel_region_cap.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace ta_roienc {

namespace {

class VaapiEngine final : public LibavEngine {
public:
    Engine kind() const override { return Engine::Vaapi; }

    EngineAbilities GetCapabilities(const AbilityQuery& query) const override
    {
        const Coding codec = query.codec;
        EngineAbilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_areas = true;
        c.runtime_roi_update = true;
        c.max_roi_regions = intel_max_roi_regions(codec);
        return c;
    }

    const char* codec_name(Coding codec) const override
    {
        return codec == Coding::H264 ? "h264_vaapi" : "hevc_vaapi";
    }

    Verdict configure_context(LibavRun& out, const EngineSetup& p) const override
    {
        if (!backend_common::open_vaapi_device(out.hw_device)) {
            return Verdict::DeviceUnavailable("no VA-API device on this machine");
        }
        if (!backend_common::init_hw_frames(out.hw_device, out.hw_frames,
                AV_PIX_FMT_VAAPI, AV_PIX_FMT_NV12, p.input.width, p.input.height, 20)) {
            return Verdict::DeviceUnavailable("VA-API device opened but allocates no frames");
        }

        out.enc.p->pix_fmt = AV_PIX_FMT_VAAPI;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        av_opt_set(out.enc.p->priv_data, "rc_mode", "CQP", 0);
        return Verdict::Ok();
    }
};

} // namespace

const IEngine& vaapi_backend()
{
    static const VaapiEngine instance;
    return instance;
}

} // namespace ta_roienc
