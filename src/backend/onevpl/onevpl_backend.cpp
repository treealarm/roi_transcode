// Intel oneVPL / Quick Sync -- TZ v2.0 §5.
//
// ROI reaches the hardware as mfxExtEncoderROI, which libavcodec fills from the frame's
// AVRegionOfInterest side data. Everything unusual here was found by measurement on Alder
// Lake and is commented where it happens; none of it follows from the documentation.

#include "backend/common/backend_common.h"
#include "backend/common/intel_roi_limit.h"
#include "preset.h"

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace sve {

namespace {

class OneVplBackend final : public LibavBackend {
public:
    Encoder kind() const override { return Encoder::Qsv; }

    EncoderCapabilities GetCapabilities(Codec codec) const override
    {
        EncoderCapabilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_rectangles = true;
        c.runtime_roi_update = true;
        c.max_roi_regions = intel_max_roi_regions(codec);
        return c;
    }

    // QSV opens on hardware it cannot actually encode with: the context is created, the
    // first frame fails. Hence a probe, which is the only way to tell the two apart before
    // committing a whole clip to it.
    bool NeedsProbe() const override { return true; }

    const char* codec_name(Codec codec) const override
    {
        return codec == Codec::H264 ? "h264_qsv" : "hevc_qsv";
    }

    Status configure_context(LibavSession& out, const EncoderConfig& p) const override
    {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "child_device_type", "vaapi", 0);
        const int derr = av_hwdevice_ctx_create(
            &out.hw_device.p, AV_HWDEVICE_TYPE_QSV, "auto", opts, 0);
        av_dict_free(&opts);
        if (derr < 0
            && !backend_common::open_hw_device(
                out.hw_device, AV_HWDEVICE_TYPE_QSV, nullptr)) {
            return Status::DeviceUnavailable("no QSV device on this machine");
        }

        // The encoder reads mfxFrameSurface1 out of a pool we own, rather than taking NV12
        // in system memory and letting qsvenc upload it internally. Two things follow.
        // The upload becomes ours, so it is visible in BackendSession::frames_uploaded
        // instead of happening a layer down where nothing counts it. And the pool is what
        // CreateFrame hands surfaces out of -- a factory surface has to come from the pool
        // the encoder will read, or it is just another buffer to copy from.
        if (!backend_common::init_hw_frames(out.hw_device, out.hw_frames, AV_PIX_FMT_QSV,
                AV_PIX_FMT_NV12, p.input.width, p.input.height, 24)) {
            return Status::DeviceUnavailable("QSV device opened but allocates no surfaces");
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_QSV;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = AV_PIX_FMT_NV12;
        out.hw_upload = true;

        // Alder Lake encodes through EncSliceLP only: VDENC + CQP. ICQ and global_quality
        // fail there, and CQP is also the one mode where mfxExtEncoderROI DeltaQP applies
        // -- so this holds for H.264 too, not just HEVC.
        av_opt_set(out.enc.p->priv_data, "low_power", "1", 0);
        if (p.codec == Codec::H265) {
            // Generalized P/B frames: an HEVC-only option. h264_qsv would reject it, and
            // asking for something that does not exist is worth not doing.
            av_opt_set(out.enc.p->priv_data, "gpb", "0", 0);
        }
        av_opt_set(out.enc.p->priv_data, "preset", qsv_preset(p.preset), 0);
        out.enc.p->flags |= AV_CODEC_FLAG_QSCALE;
        out.enc.p->global_quality = p.crf * FF_QP2LAMBDA;
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& onevpl_backend()
{
    static const OneVplBackend instance;
    return instance;
}

} // namespace sve
