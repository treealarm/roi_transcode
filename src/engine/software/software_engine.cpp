// libx264 and libx265 through libavcodec -- two backends, not one.
//
// libx264 and libx265 are separate libraries with separate APIs and separate capabilities,
// and a caller who lists "the CPU" in a preference order has not actually said which of the
// two they mean. So they are two backends, and a mismatch between the backend named and the
// codec asked for is answered rather than absorbed.
//
// What the two share is everything below the API: both reach the encoder through the same
// libavcodec pair, both take offsets as a per-macroblock quantOffsets map -- which is what
// makes the region count unlimited, unlike the Intel path -- and both ignore those offsets
// outright when aq-mode is 0, which is why it is set here and not left to the preset. So
// the implementation is one class with the codec fixed at construction.

#include "engine/common/engine_common.h"
#include "effort.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/opt.h>
}

namespace ta_roienc {

namespace {

class SoftwareEngine final : public LibavEngine {
public:
    SoftwareEngine(Engine kind, Coding codec, const char* engine_name)
        : m_kind(kind), m_codec(codec), m_name(engine_name)
    {
    }

    Engine kind() const override { return m_kind; }

    EngineAbilities GetCapabilities(const AbilityQuery& query) const override
    {
        const Coding codec = query.codec;
        EngineAbilities c;
        if (codec != m_codec) {
            // Not a degraded answer but an empty one: this backend cannot produce that
            // codec at all, and selection reads exactly this to skip it.
            return c;
        }
        c.h264 = m_codec == Coding::H264;
        c.h265 = m_codec == Coding::H265;
        c.roi_areas = true;
        // The offsets are a per-block quantOffsets map underneath, which is what makes the
        // region count unlimited: rectangles are just how FFmpeg describes them on the way
        // in. H.264 cells are macroblocks; for HEVC we paint 32x32 before merge (see
        // EngineAbilities::qp_map_block_*).
        c.qp_offsets = true;
        if (m_codec == Coding::H264) {
            c.qp_map_block_width = 16;
            c.qp_map_block_height = 16;
        } else {
            c.qp_map_block_width = 32;
            c.qp_map_block_height = 32;
        }
        c.runtime_roi_update = true;
        // Guard: CPU HEVC + ROI is unstable on this stack; keep SEI on H.264 only for now.
        c.sei = m_codec == Coding::H264;
        // Native bitrate path is currently H.264-only: libx265 + ROI + target_bitrate
        // is unstable on our stack, so HEVC stays on CRF/CQP + outer BitAllowance.
        c.native_bitrate = m_codec == Coding::H264;
        c.max_roi_regions = 0;  // no limit worth naming

        // The ten-bit profile here is HEVC's, so ten bits are only claimed for H.265 even
        // where the H.264 encoder would take them.
        if (m_codec == Coding::H265) {
            const auto ten = backend_common::query_ten_bit(m_name);
            c.main10 = ten.main10;
            c.p010_input = ten.p010_input;
        }
        return c;
    }

    const char* codec_name(Coding) const override { return m_name; }

    Verdict configure_context(LibavRun& out, const EngineSetup& p) const override
    {
        if (p.input.pixel_layout == PixelLayout::P010) {
            const auto ten = backend_common::query_ten_bit(m_name);
            if (!ten.main10) {
                return Verdict::Unsupported(
                    std::string(m_name) + " in this build has no ten-bit path");
            }
            // libx265 takes yuv420p10le and not P010, so the frame is converted on the way
            // in -- counted in frames_converted like any other conversion.
            out.enc.p->pix_fmt = static_cast<AVPixelFormat>(ten.encode_format);
            out.sw_pix = static_cast<AVPixelFormat>(ten.encode_format);
        } else {
            out.enc.p->pix_fmt = AV_PIX_FMT_YUV420P;
            out.sw_pix = AV_PIX_FMT_YUV420P;
        }
        av_opt_set(out.enc.p->priv_data, "preset", libx26x_preset(p.preset), 0);
        av_opt_set(out.enc.p->priv_data, "aq-mode", "1", 0);
        // Accept AV_FRAME_DATA_SEI_UNREGISTERED as user-data-unregistered SEI.
        av_opt_set_int(out.enc.p->priv_data, "udu_sei", 1, 0);

        if (p.rate_control.target_bitrate > 0 && m_codec == Coding::H264) {
            // Native bitrate mode for H.264. Do not also set CRF.
            const int64_t target = static_cast<int64_t>(p.rate_control.target_bitrate);
            const float peak_frac = p.rate_control.max_peak_overshoot > 0.0f
                ? p.rate_control.max_peak_overshoot
                : 0.2f;
            const int64_t maxrate = std::max<int64_t>(target,
                static_cast<int64_t>(std::llround(
                    static_cast<double>(target) * (1.0 + static_cast<double>(peak_frac)))));
            const int64_t buf = std::min<int64_t>(
                target * 2, static_cast<int64_t>(INT32_MAX));
            out.enc.p->bit_rate = target;
            out.enc.p->rc_max_rate = maxrate;
            out.enc.p->rc_buffer_size = static_cast<int>(buf);
            // Filler makes undershoot meet the HRD rate so baseline≈adaptive.
            av_opt_set(out.enc.p->priv_data, "x264-params",
                "nal-hrd=cbr:filler=1", 0);
        } else {
            av_opt_set(out.enc.p->priv_data, "crf", std::to_string(p.crf).c_str(), 0);
        }
        return Verdict::Ok();
    }

private:
    Engine m_kind;
    Coding m_codec;
    const char* m_name;
};

} // namespace

const IEngine& x264_backend()
{
    static const SoftwareEngine instance(Engine::X264, Coding::H264, "libx264");
    return instance;
}

const IEngine& x265_backend()
{
    static const SoftwareEngine instance(Engine::X265, Coding::H265, "libx265");
    return instance;
}

} // namespace ta_roienc
