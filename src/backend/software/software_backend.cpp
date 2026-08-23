// libx264 / libx265 -- the fallback that is always available.
//
// Its ROI support is the most capable of the three in one respect: the offsets go in as a
// per-macroblock quantOffsets map, so there is no region count to run out of. The catch is
// that they are applied through adaptive quantisation and are ignored outright when
// aq-mode is 0 -- which is why it is set here and not left to the preset.

#include "backend/common/backend_common.h"
#include "preset.h"

#include <string>

extern "C" {
#include <libavutil/opt.h>
}

namespace smartvideo {

namespace {

class SoftwareBackend final : public LibavBackend {
public:
    Encoder kind() const override { return Encoder::Cpu; }

    EncoderCapabilities GetCapabilities(Codec) const override
    {
        EncoderCapabilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_rectangles = true;
        // The offsets are per macroblock underneath, which is what makes the region count
        // unlimited: rectangles are just how they are described on the way in.
        c.per_block_qp = true;
        c.runtime_roi_update = true;
        c.max_roi_regions = 0;  // no limit worth naming
        return c;
    }

    const char* codec_name(Codec codec) const override
    {
        return codec == Codec::H264 ? "libx264" : "libx265";
    }

    Status configure_context(LibavSession& out, const EncoderConfig& p) const override
    {
        out.enc.p->pix_fmt = AV_PIX_FMT_YUV420P;
        out.sw_pix = AV_PIX_FMT_YUV420P;
        av_opt_set(out.enc.p->priv_data, "preset", libx26x_preset(p.preset), 0);
        av_opt_set(out.enc.p->priv_data, "crf", std::to_string(p.crf).c_str(), 0);
        av_opt_set(out.enc.p->priv_data, "aq-mode", "1", 0);
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& software_backend()
{
    static const SoftwareBackend instance;
    return instance;
}

} // namespace smartvideo
