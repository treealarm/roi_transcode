// Intel oneVPL / Quick Sync -- TZ v2.0 §5.
//
// ROI reaches the hardware as mfxExtEncoderROI, which libavcodec fills from the frame's
// AVRegionOfInterest side data. Everything unusual here was found by measurement on Alder
// Lake and is commented where it happens; none of it follows from the documentation.

#include "backend/common/backend_common.h"

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace smartvideo {

namespace {

class OneVplBackend final : public IEncoderBackend {
public:
    Encoder kind() const override { return Encoder::Qsv; }

    EncoderCapabilities GetCapabilities(Codec) const override
    {
        EncoderCapabilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_rectangles = true;
        c.runtime_roi_update = true;
        // mfxExtEncoderROI carries at most 256 regions. Asking for more is not an error --
        // the extra ones are dropped strongest-first by the caller and counted.
        c.max_roi_regions = 256;
        return c;
    }

    // QSV opens on hardware it cannot actually encode with: the context is created, the
    // first frame fails. Hence a probe, which is the only way to tell the two apart before
    // committing a whole clip to it.
    bool NeedsProbe() const override { return true; }

    bool Configure(BackendSession& out, const EncoderParams& p) const override
    {
        backend_common::apply_shared(out.enc.p, p);

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "child_device_type", "vaapi", 0);
        const int derr = av_hwdevice_ctx_create(
            &out.hw_device.p, AV_HWDEVICE_TYPE_QSV, "auto", opts, 0);
        av_dict_free(&opts);
        if (derr < 0
            && !backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_QSV, nullptr)) {
            return false;
        }

        out.enc.p->pix_fmt = AV_PIX_FMT_NV12;
        out.enc.p->hw_device_ctx = av_buffer_ref(out.hw_device.p);
        out.sw_pix = AV_PIX_FMT_NV12;

        // Alder Lake encodes through EncSliceLP only: VDENC + CQP. ICQ and global_quality
        // fail there, and CQP is also the one mode where mfxExtEncoderROI DeltaQP applies
        // -- so this holds for H.264 too, not just HEVC.
        av_opt_set(out.enc.p->priv_data, "low_power", "1", 0);
        if (p.codec == Codec::H265) {
            // Generalized P/B frames: an HEVC-only option. h264_qsv would reject it, and
            // asking for something that does not exist is worth not doing.
            av_opt_set(out.enc.p->priv_data, "gpb", "0", 0);
        }
        av_opt_set(out.enc.p->priv_data, "preset", p.preset.c_str(), 0);
        out.enc.p->flags |= AV_CODEC_FLAG_QSCALE;
        out.enc.p->global_quality = p.crf * FF_QP2LAMBDA;
        return true;
    }
};

} // namespace

const IEncoderBackend& onevpl_backend()
{
    static const OneVplBackend instance;
    return instance;
}

} // namespace smartvideo
