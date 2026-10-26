// Intel oneVPL / Quick Sync through libavcodec.
//
// ROI reaches the hardware as mfxExtEncoderROI, which libavcodec fills from the frame's
// AVRegionOfInterest side data. Everything unusual here was found by measurement on Alder
// Lake and is commented where it happens; none of it follows from the documentation.

#include "engine/common/av_hw_device.h"
#include "engine/common/engine_common.h"
#include "engine/common/intel_region_cap.h"
#include "effort.h"

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace ta_roienc {

namespace {

/// libavutil's spelling of a pool that grows on demand.
constexpr int kDynamicPool = 0;

/// Intel's PCI vendor id, which DXGI reports as the adapter's VendorId.
constexpr const char* kIntelVendorId = "0x8086";

/// The device QSV runs on top of: Direct3D 11 where this FFmpeg has it -- Windows -- and
/// VA-API everywhere else. Opened here rather than left to QSV to make for itself, because
/// it is also the device a decoder in front of this encoder is handed (LibavRun::
/// decode_device): no decoder in libavcodec decodes into QSV surfaces, every one of them
/// decodes into D3D11 or VA-API ones, and those map into QSV's without a copy.
AVHWDeviceType child_device_type() noexcept
{
    return hw_device_type_present(AV_HWDEVICE_TYPE_D3D11VA) ? AV_HWDEVICE_TYPE_D3D11VA
                                                            : AV_HWDEVICE_TYPE_VAAPI;
}

bool open_child_device(roiav::ByteBlock& hw, AVHWDeviceType type)
{
    return type == AV_HWDEVICE_TYPE_D3D11VA
        ? backend_common::open_d3d11_device(hw, kIntelVendorId)
        : backend_common::open_vaapi_device(hw);
}

class OneVplEngine final : public LibavEngine {
public:
    Engine kind() const override { return Engine::IntelOneVpl; }

    EngineAbilities GetCapabilities(const AbilityQuery& query) const override
    {
        const Coding codec = query.codec;
        EngineAbilities c;
        c.h264 = true;
        c.h265 = true;
        c.roi_areas = true;
        c.runtime_roi_update = true;
        c.max_roi_regions = intel_max_roi_regions(codec);
        if (codec == Coding::H265) {
            const auto ten = backend_common::query_ten_bit("hevc_qsv");
            c.main10 = ten.main10;
            c.p010_input = ten.p010_input;
        }
        return c;
    }

    // QSV opens on hardware it cannot actually encode with: the context is created, the
    // first frame fails. Hence a probe, which is the only way to tell the two apart before
    // committing a whole clip to it.
    bool NeedsProbe() const override { return true; }

    const char* codec_name(Coding codec) const override
    {
        return codec == Coding::H264 ? "h264_qsv" : "hevc_qsv";
    }

    Verdict configure_context(LibavRun& out, const EngineSetup& p) const override
    {
        const AVHWDeviceType child_type = child_device_type();
        const char* child_name = av_hwdevice_get_type_name(child_type);
        if (!open_child_device(out.decode_device, child_type)) {
            return Verdict::DeviceUnavailable(std::string("no Intel ") + child_name
                + " device: no Intel GPU, or no driver for it");
        }
        av_buffer_unref(&out.hw_device.p);
        if (av_hwdevice_ctx_create_derived(
                &out.hw_device.p, AV_HWDEVICE_TYPE_QSV, out.decode_device.p, 0) < 0) {
            return Verdict::DeviceUnavailable(std::string("no QSV runtime on the Intel ")
                + child_name + " device");
        }

        // The encoder reads mfxFrameSurface1 out of a pool we own, rather than taking NV12
        // in system memory and letting qsvenc upload it internally. Two things follow.
        // The upload becomes ours, so it is visible in EngineRun::frames_uploaded
        // instead of happening a layer down where nothing counts it. And the pool is what
        // CreatePicture hands surfaces out of -- a factory surface has to come from the pool
        // the encoder will read, or it is just another buffer to copy from.
        // The surface pool's own format follows the input's depth: eight bits ride in NV12,
        // ten in P010, and Quick Sync takes both directly.
        const bool ten_bit = p.input.pixel_layout == PixelLayout::P010;
        const AVPixelFormat surface = ten_bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        if (ten_bit && !backend_common::query_ten_bit(codec_name(p.codec)).main10) {
            return Verdict::Unsupported(
                std::string(codec_name(p.codec)) + " on this device has no ten-bit path");
        }
        // A dynamic pool, which makes its surfaces one at a time, and not for economy:
        // qsvenc looks every surface of a fixed pool up among the ones that pool allocated,
        // so a decoder's frame mapped into one is refused. From a dynamic pool it takes any.
        if (!backend_common::init_hw_frames(out.hw_device, out.hw_frames, AV_PIX_FMT_QSV,
                surface, p.input.width, p.input.height, kDynamicPool)) {
            return Verdict::DeviceUnavailable("QSV device opened but allocates no surfaces");
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_QSV;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = surface;
        out.hw_upload = true;

        // Alder Lake encodes through EncSliceLP only: VDENC + CQP. ICQ and global_quality
        // fail there, and CQP is also the one mode where mfxExtEncoderROI DeltaQP applies
        // -- so this holds for H.264 too, not just HEVC.
        av_opt_set(out.enc.p->priv_data, "low_power", "1", 0);
        if (p.codec == Coding::H265) {
            // Generalized P/B frames: an HEVC-only option. h264_qsv would reject it, and
            // asking for something that does not exist is worth not doing.
            av_opt_set(out.enc.p->priv_data, "gpb", "0", 0);
        }
        av_opt_set(out.enc.p->priv_data, "preset", qsv_preset(p.preset), 0);
        out.enc.p->flags |= AV_CODEC_FLAG_QSCALE;
        out.enc.p->global_quality = p.crf * FF_QP2LAMBDA;
        return Verdict::Ok();
    }
};

} // namespace

const IEngine& onevpl_backend()
{
    static const OneVplEngine instance;
    return instance;
}

} // namespace ta_roienc
