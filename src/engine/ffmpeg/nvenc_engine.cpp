// NVIDIA NVENC through libavcodec, on a CUDA device.
//
// Nothing of NVIDIA's is linked: libavcodec loads the driver's own libraries
// (nvEncodeAPI64.dll / libnvidia-encode.so, nvcuda.dll / libcuda.so) when the device opens,
// so a machine without the card -- or an FFmpeg built without nvcodec -- declines here like
// any absent device rather than failing to load.
//
// It takes no regions. libavcodec's nvenc reads no AVRegionOfInterest (NVENC's own QP delta
// map is not wired up there), so the capabilities say so, and a caller that needs regions
// is steered to a backend that applies them instead of having them silently ignored.
//
// Which is also why it holds a bitrate target with its own rate control. The outer loop's
// actuator is a quantiser offset on the background, carried as a region -- here it would move
// nothing, and a 1080p25 test pattern at QP 23 went out at 10 Mbit/s against a 3.9 Mbit/s target.

#include "engine/common/bitrate_control.h"
#include "engine/common/engine_common.h"
#include "effort.h"

#include <algorithm>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace ta_roienc {

namespace {

/// Frames the upload pool starts with: the one being uploaded plus what the encoder may still
/// hold while it works, with room to spare. CUDA pools grow past it on demand, so this is a
/// starting size rather than a ceiling.
constexpr int kUploadPool = 20;

class NvencEngine final : public LibavEngine {
public:
    Engine kind() const override { return Engine::Nvenc; }

    EngineAbilities GetCapabilities(const AbilityQuery& query) const override
    {
        EngineAbilities c;
        c.h264 = true;
        c.h265 = true;
        // VBR at the target when one is asked for, CQP otherwise. Without regions there is
        // nothing CQP would keep that VBR gives up, unlike on Intel.
        c.native_bitrate = true;
        if (query.codec == Coding::H265) {
            const auto ten = backend_common::query_ten_bit("hevc_nvenc");
            c.main10 = ten.main10;
            c.p010_input = ten.p010_input;
        }
        return c;
    }

    const char* codec_name(Coding codec) const override
    {
        return codec == Coding::H264 ? "h264_nvenc" : "hevc_nvenc";
    }

    Verdict configure_context(LibavRun& out, const EngineSetup& p) const override
    {
        if (!backend_common::open_hw_device(out.hw_device, AV_HWDEVICE_TYPE_CUDA, nullptr)) {
            return Verdict::DeviceUnavailable("no CUDA device: no NVIDIA card, or no driver for it");
        }

        // The encoder reads CUDA frames out of a pool we own, so the upload from system memory
        // is ours and counted (EngineRun::frames_uploaded), and a decoder given this device
        // hands over frames that are already there -- NVDEC to NVENC without leaving the card.
        const bool ten_bit = p.input.pixel_layout == PixelLayout::P010;
        const AVPixelFormat surface = ten_bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        if (ten_bit && !backend_common::query_ten_bit(codec_name(p.codec)).main10) {
            return Verdict::Unsupported(
                std::string(codec_name(p.codec)) + " in this FFmpeg has no ten-bit path");
        }
        if (!backend_common::init_hw_frames(out.hw_device, out.hw_frames, AV_PIX_FMT_CUDA,
                surface, p.input.width, p.input.height, kUploadPool)) {
            return Verdict::DeviceUnavailable("CUDA device opened but allocates no frames");
        }
        out.enc.p->pix_fmt = AV_PIX_FMT_CUDA;
        out.enc.p->hw_frames_ctx = av_buffer_ref(out.hw_frames.p);
        out.sw_pix = surface;
        out.hw_upload = true;

        av_opt_set(out.enc.p->priv_data, "preset", nvenc_preset(p.preset), 0);
        if (p.rate_control.target_bitrate > 0) {
            // The same plan the native oneVPL backend opens VBR with: the ceiling at the target
            // and one second of it as the buffer.
            const BufferPlan vbv = vbv_plan_for(p.rate_control.target_bitrate);
            av_opt_set(out.enc.p->priv_data, "rc", "vbr", 0);
            out.enc.p->bit_rate = static_cast<int64_t>(vbv.bitrate_kbps) * kBitsPerKilobit;
            out.enc.p->rc_max_rate = static_cast<int64_t>(vbv.max_kbps) * kBitsPerKilobit;
            out.enc.p->rc_buffer_size = static_cast<int>(std::min<int64_t>(
                static_cast<int64_t>(vbv.buffer_kbits) * kBitsPerKilobit, INT32_MAX));
        } else {
            // Constant QP at crf, as VA-API runs: the quantiser asked for is the quantiser used.
            av_opt_set(out.enc.p->priv_data, "rc", "constqp", 0);
            av_opt_set_int(out.enc.p->priv_data, "qp", p.crf, 0);
        }
        // A frame in, a packet out. NVENC otherwise keeps a few frames in flight to keep the
        // engine busy, which is latency on a live stream and buys nothing without B-frames,
        // which apply_shared turns off for every backend.
        av_opt_set(out.enc.p->priv_data, "tune", "ll", 0);
        av_opt_set_int(out.enc.p->priv_data, "delay", 0, 0);
        av_opt_set_int(out.enc.p->priv_data, "zerolatency", 1, 0);
        return Verdict::Ok();
    }
};

} // namespace

const IEngine& nvenc_backend()
{
    static const NvencEngine instance;
    return instance;
}

} // namespace ta_roienc
