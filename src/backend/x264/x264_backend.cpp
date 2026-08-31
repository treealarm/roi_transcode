// libx264 through its own API -- TZ v4.1 §5.4.
//
// The first backend here that does not go through libavcodec. What that buys is not speed
// and not independence for its own sake; it is the one thing this library exists to do.
//
// Today's software path hands regions to libavcodec as AV_FRAME_DATA_REGIONS_OF_INTEREST,
// whose qoffset is documented as a *fraction of the quantiser range*, interpolated
// "in some codec-dependent way". So a qp_delta of -10 leaves here as -10/51 of a range and
// libavcodec decides what that means. libx264's own control is quant_offsets: one float per
// macroblock, "added on top of the decisions made by x264", in QP units. Our qp_delta is
// already exactly that, so the direct path carries the number the policy computed instead
// of a fraction standing in for it.
//
// Three facts about quant_offsets, all from the pinned header, all of them load-bearing:
//
//   * The grid is one float per 16x16 macroblock in raster order, with width and height
//     rounded up to the nearest 16. Not the frame's block count -- the rounded one.
//   * Positive raises QP, which is worse quality. Same sign as our qp_delta, so nothing is
//     negated on the way in. Getting this backwards would protect the background and crush
//     the plate, and both files would look plausible in a directory listing.
//   * x264 keeps the pointer until it has finished with the frame, and with lookahead that
//     is not when encode() returns. The array is therefore handed over with
//     quant_offsets_free, which is the header's own answer to this, rather than kept in the
//     session and reused -- reuse is the shape of the bug, not the fix.
//
// And the condition without which none of it does anything: "Adaptive quantization must be
// enabled to use this feature." aq_mode is set here for that reason and no other.

#include "backend/common/backend_common.h"
#include "preset.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <x264.h>
}

namespace sve {

namespace {

/// One opened libx264 encoder. Shares nothing with LibavSession, which is why
/// BackendSession is a base rather than a structure.
class X264Session final : public BackendSession {
public:
    ~X264Session() override
    {
        if (enc != nullptr) {
            x264_encoder_close(enc);
        }
    }

    x264_t* enc = nullptr;
    x264_param_t param{};
    Codec codec = Codec::H264;
    int width = 0;
    int height = 0;

    /// What ReceivePacket hands out. Owned here because x264's own NAL buffer is only
    /// valid until the next call to x264_encoder_encode, and EncodedPacket outlives that.
    std::vector<uint8_t> payload;
    int64_t pts = 0;
    int64_t dts = 0;
    bool key = false;
    bool has_packet = false;

    /// Set by Flush. Drives ReceivePacket to pull the delayed frames out and then say
    /// EndOfStream once there are none left.
    bool draining = false;

    long frames_converted = 0;
};

X264Session& x264_session(BackendSession& session)
{
    return static_cast<X264Session&>(session);
}

/// The macroblock grid, on x264's terms: "width and height are rounded up to the nearest
/// 16". Derived rather than remembered, because a session that computed it once and a
/// caller that reconfigured would disagree exactly where it hurts.
constexpr int mb_count(int pixels)
{
    return (pixels + 15) / 16;
}

/// Regions to one float per macroblock, in QP units.
///
/// Strongest first, and the first region covering a macroblock wins: §11 resolves an
/// overlap by index, and the normalizer has already ordered them. Anything no region covers
/// keeps x264's own decision, which is what an offset of zero means.
///
/// Allocated with malloc because x264 frees it through the callback below, and a pointer
/// that crossed from new[] to free() is undefined however well it happens to work.
float* build_quant_offsets(int width, int height, std::span<const Rect> regions)
{
    if (regions.empty()) {
        return nullptr;
    }
    const int mbw = mb_count(width);
    const int mbh = mb_count(height);
    const std::size_t cells = static_cast<std::size_t>(mbw) * static_cast<std::size_t>(mbh);

    auto* offsets = static_cast<float*>(std::calloc(cells, sizeof(float)));
    if (offsets == nullptr) {
        return nullptr;
    }

    // Painted back to front so that the strongest region, which comes first, is written
    // last and survives. One pass per region over its own macroblocks only -- the whole
    // grid per region would be O(regions x frame) for a background rectangle that covers
    // everything anyway.
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        const Rect& r = *it;
        const int x0 = std::clamp(r.left / 16, 0, mbw - 1);
        const int x1 = std::clamp((r.right + 15) / 16, 0, mbw);
        const int y0 = std::clamp(r.top / 16, 0, mbh - 1);
        const int y1 = std::clamp((r.bottom + 15) / 16, 0, mbh);
        const float offset = static_cast<float>(r.qp_delta);
        for (int y = y0; y < y1; ++y) {
            float* row = offsets + static_cast<std::size_t>(y) * static_cast<std::size_t>(mbw);
            std::fill(row + x0, row + x1, offset);
        }
    }
    return offsets;
}

void free_quant_offsets(void* p)
{
    std::free(p);
}

/// x264's five presets against our own -- the same mapping the libavcodec path uses, taken
/// from the same helper so the two cannot drift while both exist.
const char* preset_name_for(Preset p)
{
    return libx26x_preset(p);
}

class X264Backend final : public IEncoderBackend {
public:
    Backend kind() const override { return Backend::X264Direct; }

    EncoderCapabilities GetCapabilities(Codec codec) const override
    {
        EncoderCapabilities c;
        if (codec != Codec::H264) {
            return c;
        }
        c.h264 = true;
        c.roi_rectangles = true;
        c.qp_map = true;
        // The real grid, not an approximation of one. This is the number a caller needs to
        // know what a QP map they hand us will actually be quantised to.
        c.qp_map_block_width = 16;
        c.qp_map_block_height = 16;
        c.runtime_roi_update = true;
        // SEI goes in through x264_picture_t::extra_sei, which is a direct channel rather
        // than the udu_sei option the libavcodec path has to ask for.
        c.sei = true;
        // Not yet: the native rate control modes are a separate piece of work from getting
        // the offsets right, and claiming one that is not wired would be the silent
        // degradation §50 forbids.
        c.native_bitrate = false;
        c.max_roi_regions = 0;  // a per-macroblock map has no region count to run out of
        return c;
    }

    Result<SessionPtr> Configure(const BackendConfig& config) const override
    {
        if (config.codec != Codec::H264) {
            return Status::Unsupported("the x264 backend encodes H.264 only");
        }
        if (config.input.pixel_format != PixelFormat::YUV420P) {
            // NV12 and P010 are conversions, and a conversion this backend does silently
            // is a copy the caller did not ask for. Refused until it is implemented and
            // counted like every other conversion.
            return Status::Unsupported(std::string("the x264 backend takes YUV420P; ")
                + pixel_format_name(config.input.pixel_format) + " is not wired yet");
        }

        auto session = std::make_unique<X264Session>();
        session->codec = config.codec;
        session->width = config.input.width;
        session->height = config.input.height;

        x264_param_t& p = session->param;
        if (x264_param_default_preset(&p, preset_name_for(config.preset), nullptr) < 0) {
            return Status::Error("x264_param_default_preset refused the preset");
        }

        p.i_width = config.input.width;
        p.i_height = config.input.height;
        p.i_csp = X264_CSP_I420;
        p.i_bitdepth = 8;
        p.i_log_level = X264_LOG_ERROR;

        const int fps_num = config.input.frame_rate.num > 0 ? config.input.frame_rate.num : 25;
        const int fps_den = config.input.frame_rate.den > 0 ? config.input.frame_rate.den : 1;
        p.i_fps_num = static_cast<uint32_t>(fps_num);
        p.i_fps_den = static_cast<uint32_t>(fps_den);
        // Timestamps travel in the caller's time base, so x264 is told what it is rather
        // than left to assume the frame rate. b_vfr_input makes it use them.
        p.i_timebase_num = config.input.time_base.num > 0 ? config.input.time_base.num : fps_den;
        p.i_timebase_den = config.input.time_base.den > 0
            ? static_cast<uint32_t>(config.input.time_base.den)
            : static_cast<uint32_t>(fps_num);
        p.b_vfr_input = 1;

        if (config.gop > 0) {
            p.i_keyint_max = config.gop;
        }

        // Without this the offsets below are ignored outright, which is the failure mode
        // that looks like a working encoder producing a file with no adaptation in it.
        if (p.rc.i_aq_mode == X264_AQ_NONE) {
            p.rc.i_aq_mode = X264_AQ_VARIANCE;
        }

        p.rc.i_rc_method = X264_RC_CRF;
        p.rc.f_rf_constant = static_cast<float>(config.crf);

        // Annex B with headers repeated on every keyframe: the packets go to a muxer that
        // may start mid-stream, and a decoder that never sees SPS/PPS decodes nothing.
        p.b_annexb = 1;
        p.b_repeat_headers = 1;

        if (x264_param_apply_profile(&p, "high") < 0) {
            return Status::Error("x264_param_apply_profile(high) refused");
        }

        session->enc = x264_encoder_open(&p);
        if (session->enc == nullptr) {
            // Not DeviceUnavailable: libx264 is linked in, so this is a configuration it
            // would not take, and telling the caller to try another backend would send
            // them looking for hardware that is not the problem.
            return Status::BackendError("x264_encoder_open refused this configuration");
        }
        return SessionPtr(session.release());
    }

    Result<FrameBufferPtr> CreateFrame(BackendSession&, MemoryType) const override
    {
        // libx264 reads the caller's planes directly; there is no encoder-owned pool to
        // hand out, and pretending otherwise would add a copy to the path that exists to
        // avoid one.
        return Status::Unsupported("the x264 backend has no frame pool of its own");
    }

    Status SubmitFrame(BackendSession& session, IFrameBuffer& frame,
        const BackendFrameControl& control) const override
    {
        X264Session& s = x264_session(session);
        if (s.draining) {
            return Status::InvalidArgument("SubmitFrame after Flush");
        }
        if (frame.format() != PixelFormat::YUV420P) {
            return Status::Unsupported("the x264 backend takes YUV420P");
        }

        x264_picture_t pic_in;
        x264_picture_init(&pic_in);
        pic_in.i_type = X264_TYPE_AUTO;
        pic_in.i_pts = control.pts;
        pic_in.img.i_csp = X264_CSP_I420;
        pic_in.img.i_plane = 3;
        for (int i = 0; i < 3; ++i) {
            const PlaneView plane = frame.plane(i);
            if (plane.empty()) {
                return Status::InvalidArgument("frame plane " + std::to_string(i) + " is empty");
            }
            // const_cast: x264 reads these and does not write them. The signature is not
            // const because x264 also uses this structure for its reconstructed output.
            pic_in.img.plane[i] = const_cast<uint8_t*>(plane.bytes.data());
            pic_in.img.i_stride[i] = plane.stride;
        }

        pic_in.prop.quant_offsets = build_quant_offsets(s.width, s.height, control.regions);
        if (pic_in.prop.quant_offsets != nullptr) {
            // Handed over, not lent. x264 holds the array past this call whenever lookahead
            // is on, and freeing it here would be a use-after-free that only appears under
            // settings nobody changed on purpose.
            pic_in.prop.quant_offsets_free = &free_quant_offsets;
        }

        if (!control.sei_unregistered.empty()) {
            // §19: UUID || payload, as user-data-unregistered.
            //
            // Handed over, exactly like quant_offsets and for exactly the same reason. The
            // first version of this borrowed the caller's span for the length of the call,
            // on the assumption that x264 copies the payload into the bitstream during
            // encode. It does not: with lookahead the SEI is written when the frame is
            // finally coded, which is several calls later, and the crash lands in
            // x264_sei_write during the drain -- far from the submit that caused it.
            //
            // The header's contract is exact: sei_free frees "each payload AND
            // x264_sei_payload_t", so both allocations are made with malloc and both are
            // x264's to release.
            const std::size_t bytes = control.sei_unregistered.size();
            auto* payloads = static_cast<x264_sei_payload_t*>(
                std::malloc(sizeof(x264_sei_payload_t)));
            auto* copy = static_cast<uint8_t*>(std::malloc(bytes));
            if (payloads != nullptr && copy != nullptr) {
                std::memcpy(copy, control.sei_unregistered.data(), bytes);
                payloads[0].payload = copy;
                payloads[0].payload_size = static_cast<int>(bytes);
                payloads[0].payload_type = 5;  // user_data_unregistered
                pic_in.extra_sei.num_payloads = 1;
                pic_in.extra_sei.payloads = payloads;
                pic_in.extra_sei.sei_free = &std::free;
            } else {
                // Out of memory for metadata is not a reason to drop the picture: the
                // frame still encodes, and the missing SEI is visible in the telemetry.
                std::free(payloads);
                std::free(copy);
            }
        }

        return encode_one(s, &pic_in);
    }

    Result<EncodedPacket> ReceivePacket(BackendSession& session) const override
    {
        X264Session& s = x264_session(session);
        if (s.has_packet) {
            s.has_packet = false;
            EncodedPacket out;
            out.payload = PacketBuffer::Copy(s.payload);
            out.pts = s.pts;
            out.dts = s.dts;
            out.key_frame = s.key;
            out.codec = Codec::H264;
            return out;
        }
        if (!s.draining) {
            return Status::Again();
        }
        if (x264_encoder_delayed_frames(s.enc) == 0) {
            return Status::EndOfStream();
        }
        // One delayed frame per call, so the caller's drain loop stays the same loop it
        // uses while feeding: pull until Again or EndOfStream.
        const Status s2 = encode_one(s, nullptr);
        if (!s2.ok()) {
            return s2;
        }
        if (!s.has_packet) {
            return Status::EndOfStream();
        }
        return ReceivePacket(session);
    }

    Status Flush(BackendSession& session) const override
    {
        x264_session(session).draining = true;
        return Status::Ok();
    }

    /// §8.11, answered from x264's own headers rather than from a codec context.
    ///
    /// x264_encoder_headers gives SPS and PPS as NALs; with b_annexb they arrive with start
    /// codes, which is what a caller feeding a decoder directly wants and what the muxer
    /// expects for Annex B. Asked once per call rather than cached, because a reconfigure
    /// changes them and a stale answer is a stream nothing can open.
    Result<CodecParameters> GetCodecParameters(BackendSession& session) const override
    {
        X264Session& s = x264_session(session);
        if (s.enc == nullptr) {
            return Status::InvalidArgument("no encoder is open");
        }
        CodecParameters out;
        out.codec = Codec::H264;
        out.width = s.width;
        out.height = s.height;
        out.time_base = Rational{static_cast<int32_t>(s.param.i_timebase_num),
            static_cast<int32_t>(s.param.i_timebase_den)};

        x264_nal_t* nals = nullptr;
        int count = 0;
        const int bytes = x264_encoder_headers(s.enc, &nals, &count);
        if (bytes > 0 && nals != nullptr) {
            out.extradata = PacketBuffer::Copy(
                std::span<const uint8_t>(nals[0].p_payload, static_cast<std::size_t>(bytes)));
        }
        return out;
    }

    BackendStatistics Statistics(const BackendSession& session) const override
    {
        // Both zero, and honestly so: this backend refuses anything but YUV420P rather
        // than converting it, so there is nothing here that could have been converted.
        return BackendStatistics{static_cast<const X264Session&>(session).frames_converted, 0};
    }

    // NeedsProbe stays at its default false: the probe exists because Ubuntu's older iHD
    // opens a QSV encoder and then returns empty packets. libx264 either opens or does not.

private:
    /// One call into x264, and what came back. `pic_in` null means "give me a delayed
    /// frame", which is how x264 spells draining.
    static Status encode_one(X264Session& s, x264_picture_t* pic_in)
    {
        x264_nal_t* nals = nullptr;
        int count = 0;
        x264_picture_t pic_out;
        x264_picture_init(&pic_out);

        const int bytes = x264_encoder_encode(s.enc, &nals, &count, pic_in, &pic_out);
        if (bytes < 0) {
            return Status::BackendError("x264_encoder_encode failed");
        }
        if (bytes == 0) {
            // Nothing yet: the frame is in the lookahead queue. Not an error and not a
            // packet -- the caller submits more, or drains.
            return Status::Ok();
        }

        // The NALs are contiguous in x264's own buffer and stay valid only until the next
        // call, so they are copied out here rather than pointed at.
        s.payload.assign(nals[0].p_payload, nals[0].p_payload + bytes);
        s.pts = pic_out.i_pts;
        s.dts = pic_out.i_dts;
        s.key = pic_out.b_keyframe != 0;
        s.has_packet = true;
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& x264_direct_backend()
{
    static const X264Backend instance;
    return instance;
}

} // namespace sve
