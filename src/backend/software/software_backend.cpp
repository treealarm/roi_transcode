// libx264 and libx265 -- two backends, not one -- TZ v4.1 §5.4.
//
// They were one class until 4.1, picking a library from the codec. That was convenient and
// wrong in the same way: libx264 and libx265 are separate libraries with separate APIs and
// separate capabilities, and a caller who lists "the CPU" in a preference order has not
// actually said which of the two they mean. Now they say it, and a mismatch between the
// backend they named and the codec they asked for is answered rather than absorbed.
//
// What the two still share is everything below the API: both reach the encoder through the
// same libavcodec pair, both take offsets as a per-macroblock quantOffsets map -- which is
// what makes the region count unlimited, unlike the Intel path -- and both ignore those
// offsets outright when aq-mode is 0, which is why it is set here and not left to the
// preset. So the implementation stays one class with the codec fixed at construction.

#include "backend/common/backend_common.h"
#include "preset.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/opt.h>
}

namespace sve {

namespace {

class SoftwareBackend final : public LibavBackend {
public:
    SoftwareBackend(Backend kind, Codec codec, const char* backend_name)
        : m_kind(kind), m_codec(codec), m_name(backend_name)
    {
    }

    Backend kind() const override { return m_kind; }

    EncoderCapabilities GetCapabilities(Codec codec) const override
    {
        EncoderCapabilities c;
        if (codec != m_codec) {
            // Not a degraded answer but an empty one: this backend cannot produce that
            // codec at all, and selection reads exactly this to skip it (§8.4).
            return c;
        }
        c.h264 = m_codec == Codec::H264;
        c.h265 = m_codec == Codec::H265;
        c.roi_rectangles = true;
        // The offsets are a per-block quantOffsets map underneath, which is what makes the
        // region count unlimited: rectangles are just how FFmpeg describes them on the way
        // in. H.264 cells are macroblocks; for HEVC we paint 32x32 before merge (see
        // EncoderCapabilities::qp_map_block_*).
        c.qp_map = true;
        if (m_codec == Codec::H264) {
            c.qp_map_block_width = 16;
            c.qp_map_block_height = 16;
        } else {
            c.qp_map_block_width = 32;
            c.qp_map_block_height = 32;
        }
        c.runtime_roi_update = true;
        // Guard: CPU HEVC + ROI is unstable on this stack; keep SEI on H.264 only for now.
        c.sei = m_codec == Codec::H264;
        // Native bitrate path is currently H.264-only: libx265 + ROI + target_bitrate
        // is unstable on our stack, so HEVC stays on CRF/CQP + outer BitrateBudget.
        c.native_bitrate = m_codec == Codec::H264;
        c.max_roi_regions = 0;  // no limit worth naming

        // §15.3's profile is HEVC, so ten bits are only claimed for H.265 even where the
        // H.264 encoder would take them.
        if (m_codec == Codec::H265) {
            const auto ten = backend_common::query_ten_bit(m_name);
            c.main10 = ten.main10;
            c.p010_input = ten.p010_input;
        }
        return c;
    }

    const char* codec_name(Codec) const override { return m_name; }

    Status configure_context(LibavSession& out, const BackendConfig& p) const override
    {
        if (p.input.pixel_format == PixelFormat::P010) {
            const auto ten = backend_common::query_ten_bit(m_name);
            if (!ten.main10) {
                return Status::Unsupported(
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
        // Accept AV_FRAME_DATA_SEI_UNREGISTERED as user-data-unregistered SEI (§19).
        av_opt_set_int(out.enc.p->priv_data, "udu_sei", 1, 0);

        if (p.rate_control.target_bitrate > 0 && m_codec == Codec::H264) {
            // Native bitrate mode (TZ v3 §15–16) for H.264. Do not also set CRF.
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
            // Filler makes undershoot meet the HRD rate so baseline≈adaptive (§16).
            av_opt_set(out.enc.p->priv_data, "x264-params",
                "nal-hrd=cbr:filler=1", 0);
        } else {
            av_opt_set(out.enc.p->priv_data, "crf", std::to_string(p.crf).c_str(), 0);
        }
        return Status::Ok();
    }

private:
    Backend m_kind;
    Codec m_codec;
    const char* m_name;
};

} // namespace

const IEncoderBackend& x264_backend()
{
    static const SoftwareBackend instance(Backend::X264, Codec::H264, "libx264");
    return instance;
}

const IEncoderBackend& x265_backend()
{
    static const SoftwareBackend instance(Backend::X265, Codec::H265, "libx265");
    return instance;
}

} // namespace sve
