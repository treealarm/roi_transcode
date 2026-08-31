// NVIDIA NVENC -- TZ v2.0 §4, incomplete on purpose.
//
// The context opens and encodes, but the adaptive part does not reach the hardware: NVENC
// takes its QP map through the NVENC SDK (or an emphasis map), and libavcodec's nvenc
// wrapper does not consume AV_FRAME_DATA_REGIONS_OF_INTEREST. So this encodes at a
// constant QP and ignores the regions.
//
// GetCapabilities says so rather than claiming otherwise, which is the point of having it:
// a caller that needs ROI can see it will not get it here, instead of discovering that the
// output looks the same everywhere.
//
// Finishing it needs an NVIDIA card to develop against; §4 also asks for the adaptive
// mechanism to be chosen by capability query across GPU generations, which cannot be
// written blind.

#include "backend/common/backend_common.h"
#include "preset.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace sve {

namespace {

class NvencBackend final : public LibavBackend {
public:
    Backend kind() const override { return Backend::NvidiaNvenc; }

    EncoderCapabilities GetCapabilities(Codec codec) const override
    {
        EncoderCapabilities c;
        // Asked of the build, not declared. This backend said it could produce both codecs
        // whatever FFmpeg had been compiled with, and on a build without the nvcodec
        // feature -- which is this one -- that made the capability report say yes and
        // Configure then say "h264_nvenc: not in this FFmpeg build". Selection was walking
        // to a backend that could never open, and the report was telling a caller they
        // had NVENC when nothing of the sort was linked.
        //
        // The same rule as ten-bit in §48's AT-009: a capability is something to find out,
        // not something to write down. What this cannot answer is whether a card is
        // present -- that is a property of the machine, discovered at Configure, and
        // DeviceUnavailable is the right answer there.
        const bool built = avcodec_find_encoder_by_name(codec_name(codec)) != nullptr;
        c.h264 = built && codec == Codec::H264;
        c.h265 = built && codec == Codec::H265;
        // False until the QP map is actually submitted. The hardware supports it; this
        // code does not.
        c.roi_rectangles = false;
        c.qp_map = false;
        c.max_roi_regions = 0;
        return c;
    }

    const char* codec_name(Codec codec) const override
    {
        return codec == Codec::H264 ? "h264_nvenc" : "hevc_nvenc";
    }

    Status configure_context(LibavSession& out, const BackendConfig& p) const override
    {
        // CUDA is optional here: nvenc also takes system-memory frames, and refusing to
        // open without a CUDA context would rule out machines where it is simply absent.
        backend_common::open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_CUDA, nullptr);
        if (out.hw_device.p) {
            out.enc.p->hw_device_ctx = av_buffer_ref(out.hw_device.p);
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_NV12;
        out.sw_pix = AV_PIX_FMT_NV12;
        av_opt_set(out.enc.p->priv_data, "preset", nvenc_preset(p.preset), 0);
        av_opt_set(out.enc.p->priv_data, "rc", "constqp", 0);
        av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        // Both off: they redistribute quality on their own criteria, which is precisely
        // the decision this library is supposed to be making.
        av_opt_set(out.enc.p->priv_data, "spatial-aq", "0", 0);
        av_opt_set(out.enc.p->priv_data, "temporal-aq", "0", 0);
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& nvenc_backend()
{
    static const NvencBackend instance;
    return instance;
}

} // namespace sve
