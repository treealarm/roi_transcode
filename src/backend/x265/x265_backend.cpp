// libx265 through its own API -- TZ v4.1 §5.4.
//
// The second direct backend, and the one that shows why §5.4 says "Backend'ы не обязаны
// иметь одинаковые grid, диапазон, знак или единицы offsets". Against x264 it differs in
// three ways that all had to be found in the header rather than assumed from the sibling.
//
// The grid is not the coding unit. x265 codes in CTUs up to 64x64, but the header is
// explicit that offsets "should be given for each 16x16 block (8x8 block, when qg-size is
// 8)" -- so the array is sized on 16x16 like x264's, except when qgSize is 8. Our
// libavcodec-backed HEVC path has been declaring 32x32 in its capabilities, which was a
// guess about CTU merging and is not what libx265 actually reads.
//
// The lifetime is undocumented for the arrays themselves, and that is the finding rather
// than an oversight to work around. x264 gives quant_offsets_free and sei_free; x265's
// x265_picture has a bare `float *quantOffsets` and an x265_sei with no callback at all,
// so the header says nothing about who owns either or for how long. Guessing "it copies
// during encode" is exactly the guess that cost a crash on the x264 side, where the header
// did say otherwise. So neither is freed while x265 might still read it.
//
// What the header *does* document is the signal that says when that moment has passed:
//
//     void* userData;
//     /* The value provided on input is returned with the same picture (POC) on output */
//
// A token goes in with the picture and comes back out with it, so the held data is keyed by
// that token and dropped when its own picture emerges -- output being the terminal event
// for a picture, after analysis has read the offsets and after the SEI has been written.
// Nothing is guessed and nothing is sized: what is in flight is whatever x265 has not
// handed back yet.
//
// This replaces a ring sized from lookaheadDepth + bframes + frameNumThreads, and that ring
// was wrong. It assumed a frame recycled after that many submissions was further behind
// than anything x265 could still be holding; measurement said otherwise. Thirty frames
// passed and sixty aborted -- heap corruption that surfaced far from its cause, in the
// demuxer reading the *input* file, because the freed offsets were reused by an unrelated
// allocation. See STATUS.md, the finding recorded with slice G.
//
// The return convention differs too: x264_encoder_encode returns a byte count, x265's
// returns 1 when a picture came out and 0 while the pipeline is filling. Reading it as a
// size would treat every output as a one-byte packet.

// The public interface and nothing else. This used to include backend_common.h, which
// exists for the libavcodec backends and pulls libavcodec in behind it -- invisible while
// everything was one target, and the first thing the §5.5 split refused to compile. All
// this backend ever needed from it was BackendSession, which sve/encoder_backend.h
// declares.
#include "backend/common/rate_control.h"
#include "sve/backend_plugin.h"
#include "sve/backend_registry.h"
#include "sve/encoder_backend.h"
#include "preset.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <x265.h>
}

namespace sve {

namespace {
/// One frame's worth of what has to outlive the submit call: the offsets x265 reads through
/// a bare pointer, and the SEI bytes we write ourselves once the picture comes back out.
struct HeldFrameData {
    std::vector<float> offsets;
    std::vector<uint8_t> sei;
};

class X265Session final : public BackendSession {
public:
    ~X265Session() override
    {
        if (enc != nullptr) {
            x265_encoder_close(enc);
        }
    }

    x265_encoder* enc = nullptr;
    x265_param param{};
    int width = 0;
    int height = 0;
    /// 16 normally, 8 when qgSize is 8 -- read from the header's own rule, not from the CTU
    /// size, which is a different number entirely.
    int offset_block = 16;

    /// In-flight frame data, keyed by the token x265 round-trips through x265_picture's
    /// userData. An entry lives from submit until its own picture comes back out.
    std::unordered_map<uint64_t, HeldFrameData> held;
    /// Starts at zero and pre-increments, so a live token is never zero -- which is what
    /// makes an unset userData on output distinguishable from a real one.
    uint64_t last_token = 0;

    std::vector<uint8_t> payload;
    int64_t pts = 0;
    int64_t dts = 0;
    bool key = false;
    bool has_packet = false;
    bool draining = false;
    /// Set when a flush call came back with no picture at all, which is x265's own signal
    /// that the pipeline is empty.
    bool flush_done = false;
};

X265Session& x265_session(BackendSession& session)
{
    return static_cast<X265Session&>(session);
}

class X265Backend final : public IEncoderBackend {
public:
    Backend kind() const override { return Backend::X265Direct; }

    EncoderCapabilities GetCapabilities(const CapabilityQuery& query) const override
    {
        const Codec codec = query.codec;
        EncoderCapabilities c;
        if (codec != Codec::H265) {
            return c;
        }
        c.h265 = true;
        c.roi_rectangles = true;
        c.qp_map = true;
        // What libx265 actually reads, per its own header. The libavcodec path declares
        // 32x32 here; that was inferred from the CTU size and is not the grid quantOffsets
        // is indexed on.
        c.qp_map_block_width = 16;
        c.qp_map_block_height = 16;
        c.runtime_roi_update = true;
        c.sei = true;
        // Claimed only once it was measured, which is what Р5 asked for in as many words:
        // "вернуть SEI и native bitrate можно только отдельным срезом с regression-
        // покрытием, не тихой сменой поведения".
        //
        // What Р5 was guarding against was heap corruption on the ROI+SEI path -- "free():
        // invalid size", aborts on real clips. That was diagnosed in slice H1 and it was
        // not ours: x265 4.1 allocates its per-frame SEI buffer once and writes into it
        // without a size check (copyUserSEIMessages). This backend has written the SEI NAL
        // itself ever since, so the defect Р5 named is gone rather than avoided.
        c.native_bitrate = true;
        c.max_roi_regions = 0;
        return c;
    }

    Result<SessionPtr> Configure(const BackendConfig& config) const override
    {
        if (config.codec != Codec::H265) {
            return Status::Unsupported("the x265 backend encodes HEVC only");
        }
        if (config.input.pixel_format != PixelFormat::YUV420P) {
            return Status::Unsupported(std::string("the x265 backend takes YUV420P; ")
                + pixel_format_name(config.input.pixel_format) + " is not wired yet");
        }

        auto session = std::make_unique<X265Session>();
        session->width = config.input.width;
        session->height = config.input.height;

        x265_param& p = session->param;
        if (x265_param_default_preset(&p, libx26x_preset(config.preset), nullptr) < 0) {
            return Status::Error("x265_param_default_preset refused the preset");
        }

        p.sourceWidth = config.input.width;
        p.sourceHeight = config.input.height;
        p.internalCsp = X265_CSP_I420;
        p.internalBitDepth = 8;
        p.logLevel = X265_LOG_NONE;

        const int fps_num = config.input.frame_rate.num > 0 ? config.input.frame_rate.num : 25;
        const int fps_den = config.input.frame_rate.den > 0 ? config.input.frame_rate.den : 1;
        p.fpsNum = static_cast<uint32_t>(fps_num);
        p.fpsDenom = static_cast<uint32_t>(fps_den);

        if (config.gop > 0) {
            p.keyframeMax = config.gop;
        }

        // Same precondition as x264: without adaptive quantisation the offsets are ignored
        // and the encoder looks like it is working. x265 defaults this on, which is why it
        // is checked rather than set blindly -- a default that changes should be noticed.
        if (p.rc.aqMode == X265_AQ_NONE) {
            p.rc.aqMode = X265_AQ_AUTO_VARIANCE;
        }
        if (config.rate_control.target_bitrate > 0) {
            // ABR for the average, VBV for the peak -- the same pair as the x264 backend
            // and for the same reason: §16 measures a one-second rolling window, and an
            // average-only mode is free to spend a burst anywhere inside the clip.
            const VbvPlan vbv = vbv_plan_for(config.rate_control.target_bitrate);
            p.rc.rateControlMode = X265_RC_ABR;
            p.rc.bitrate = vbv.bitrate_kbps;
            p.rc.vbvMaxBitrate = vbv.max_kbps;
            p.rc.vbvBufferSize = vbv.buffer_kbits;
        } else {
            p.rc.rateControlMode = X265_RC_CRF;
            p.rc.rfConstant = static_cast<double>(config.crf);
        }

        p.bAnnexB = 1;
        p.bRepeatHeaders = 1;

        session->enc = x265_encoder_open(&p);
        if (session->enc == nullptr) {
            return Status::BackendError("x265_encoder_open refused this configuration");
        }

        // What x265 settled on, which is not always what was asked for. Both numbers below
        // are read from here rather than from the request.
        x265_param effective{};
        x265_encoder_parameters(session->enc, &effective);
        session->offset_block = effective.rc.qgSize == 8 ? 8 : 16;

        return SessionPtr(session.release());
    }

    Result<FrameBufferPtr> CreateFrame(BackendSession&, MemoryType) const override
    {
        return Status::Unsupported("the x265 backend has no frame pool of its own");
    }

    Status SubmitFrame(BackendSession& session, IFrameBuffer& frame,
        const BackendFrameControl& control) const override
    {
        X265Session& s = x265_session(session);
        if (s.draining) {
            return Status::InvalidArgument("SubmitFrame after Flush");
        }
        if (frame.format() != PixelFormat::YUV420P) {
            return Status::Unsupported("the x265 backend takes YUV420P");
        }

        x265_picture pic_in;
        x265_picture_init(&s.param, &pic_in);
        pic_in.pts = control.pts;
        pic_in.bitDepth = 8;
        pic_in.colorSpace = X265_CSP_I420;
        for (int i = 0; i < 3; ++i) {
            const PlaneView plane = frame.plane(i);
            if (plane.empty()) {
                return Status::InvalidArgument("frame plane " + std::to_string(i) + " is empty");
            }
            pic_in.planes[i] = const_cast<uint8_t*>(plane.bytes.data());
            pic_in.stride[i] = plane.stride;
        }

        HeldFrameData* slot = nullptr;
        const uint64_t token = claim_slot(s, &slot);
        pic_in.userData = token_to_user_data(token);
        HeldFrameData& held = *slot;
        // Always handed over, even with nothing to say -- an all-zero grid, which is what
        // "no offset" means to x265 anyway ("added on top of the decisions made by x265").
        //
        // Never conditionally, and this is a crash rather than a tidiness point. x265
        // allocates a frame's m_quantOffsets lazily, when a picture carrying offsets first
        // reaches it, and it recycles frames through a pool. A clip whose early frames
        // carry no regions therefore fills that pool with frames that have no array, and
        // the first later picture that does carry offsets memcpy's into a null pointer.
        // Measured: ASan puts the destination at 0x0 with a length of 0x3840, which is
        // exactly 3600 floats -- our 80x45 grid for 1280x720.
        //
        // It reproduces on any clip whose regions come and go, which testdata/roi.json does
        // (its object lives 0.25 s of twelve). It went unseen because it needs enough
        // frames for the pool to recycle, and it was the target-bitrate path that first
        // ran long enough to get there.
        build_offsets(s, control.regions, held.offsets);
        pic_in.quantOffsets = held.offsets.data();
        // Kept here and written by us on the way out -- see write_prefix_sei and the header
        // comment. Not handed to x265: its own user-SEI path corrupts the heap.
        held.sei.assign(control.sei_unregistered.begin(), control.sei_unregistered.end());

        return encode_one(s, &pic_in);
    }

    Result<EncodedPacket> ReceivePacket(BackendSession& session) const override
    {
        X265Session& s = x265_session(session);
        if (s.has_packet) {
            s.has_packet = false;
            EncodedPacket out;
            out.payload = PacketBuffer::Copy(s.payload);
            out.pts = s.pts;
            out.dts = s.dts;
            out.key_frame = s.key;
            out.codec = Codec::H265;
            // The unit s.pts is counted in, and the muxer has nothing else to go on:
            // EncodedPacket::time_base defaults to 1/1, so leaving it unset does not fail
            // loudly -- it silently declares one tick per second. That is what both direct
            // backends did until 2026-08-31, and a 25 fps clip muxed out as 300 seconds
            // instead of 12. The libavcodec path has always set this (backend_common.cpp);
            // packet_sink.h states the contract, that a packet carries its own time base.
            out.time_base = Rational{static_cast<int32_t>(s.param.fpsDenom),
                static_cast<int32_t>(s.param.fpsNum)};
            return out;
        }
        if (!s.draining) {
            return Status::Again();
        }
        // x265's flush protocol is "call until it stops returning pictures", so the end is
        // a call that produced none -- not a call that produced no packet. Those are
        // different: a picture can come back carrying no NALs, and reading that as the end
        // would cut the stream short with frames still inside. s.flush_done is set only by
        // the first, which is the condition the header actually documents.
        while (!s.flush_done) {
            const Status pulled = encode_one(s, nullptr);
            if (!pulled.ok()) {
                return pulled;
            }
            if (s.has_packet) {
                return ReceivePacket(session);
            }
        }
        return Status::EndOfStream();
    }

    Status Flush(BackendSession& session) const override
    {
        x265_session(session).draining = true;
        return Status::Ok();
    }

    Result<CodecParameters> GetCodecParameters(BackendSession& session) const override
    {
        X265Session& s = x265_session(session);
        if (s.enc == nullptr) {
            return Status::InvalidArgument("no encoder is open");
        }
        CodecParameters out;
        out.codec = Codec::H265;
        out.width = s.width;
        out.height = s.height;
        // x265 has no separate time base: timestamps are in frames unless the caller
        // rescales, so the frame rate is the time base and saying otherwise would move
        // every PTS.
        out.time_base = Rational{static_cast<int32_t>(s.param.fpsDenom),
            static_cast<int32_t>(s.param.fpsNum)};

        x265_nal* nals = nullptr;
        uint32_t count = 0;
        const int bytes = x265_encoder_headers(s.enc, &nals, &count);
        if (bytes > 0 && nals != nullptr) {
            out.extradata = PacketBuffer::Copy(
                std::span<const uint8_t>(nals[0].payload, static_cast<std::size_t>(bytes)));
        }
        return out;
    }

    /// Read back from the encoder, for the same reason the block size above is: a preset
    /// moves several of these, and what a calibration profile has to pin is what ran.
    EffectiveSettings GetEffectiveSettings(const BackendSession& session) const override
    {
        const X265Session& s = static_cast<const X265Session&>(session);
        EffectiveSettings out;
        if (s.enc == nullptr) {
            return out;
        }
        x265_param p{};
        x265_encoder_parameters(s.enc, &p);
        out.b_frames = p.bframes;
        out.lookahead_frames = p.lookaheadDepth;
        out.spatial_aq = p.rc.aqMode != X265_AQ_NONE;
        // cu-tree is x265's spelling of the same idea as x264's mb-tree.
        out.temporal_aq = p.rc.cuTree != 0;
        return out;
    }

    BackendStatistics Statistics(const BackendSession&) const override
    {
        // Nothing is converted here: anything but YUV420P is refused rather than converted
        // behind the caller's back.
        return BackendStatistics{};
    }

private:
    /// Our SEI as a HEVC prefix SEI NAL, in the Annex B form x265 emits around it.
    ///
    /// Written here rather than handed to x265_picture::userSEI, and that is not a
    /// preference. x265 4.1's Encoder::copyUserSEIMessages allocates the per-frame payload
    /// buffer once:
    ///
    ///     if (!frame->m_userSEI.payloads[i].payload)
    ///         frame->m_userSEI.payloads[i].payload = new uint8_t[input.payloadSize];
    ///     memcpy(frame->m_userSEI.payloads[i].payload, input.payload, input.payloadSize);
    ///
    /// -- no size check at all. Frames come from a pool, so the buffer is sized by the
    /// first payload that frame object ever sees and every later payload is copied into it
    /// whatever its length. Our payload grows and shrinks with the object count, so a frame
    /// that first carried two objects and later carries six overruns the heap. 4.3 added a
    /// grow check; we link 4.1. Confirmed by AddressSanitizer: a 52-byte write into a
    /// 25-byte region, allocated by an earlier call to that same function.
    ///
    /// Making our payload a fixed size would work around it, but the fixed size would have
    /// to be the worst case the object cap allows -- kilobytes on every frame where tens of
    /// bytes are used. Writing the NAL is both cheaper and the thing a backend that owns
    /// its bitstream should be doing anyway.
    static void append_emulation_protected(std::vector<uint8_t>& out,
        std::span<const uint8_t> rbsp)
    {
        int zeros = 0;
        for (const uint8_t b : rbsp) {
            // Any 00 00 0x with x <= 3 would read as a start code, so an escape byte goes
            // in front of it. The escape itself resets the run.
            if (zeros == 2 && b <= 0x03) {
                out.push_back(0x03);
                zeros = 0;
            }
            out.push_back(b);
            zeros = b == 0x00 ? zeros + 1 : 0;
        }
    }

    static std::vector<uint8_t> prefix_sei_nal(std::span<const uint8_t> payload)
    {
        // payloadType 5, user_data_unregistered, and payloadSize in the ff-then-remainder
        // form the standard uses for both.
        std::vector<uint8_t> rbsp;
        rbsp.reserve(payload.size() + 8);
        rbsp.push_back(0x05);
        std::size_t remaining = payload.size();
        while (remaining >= 0xFF) {
            rbsp.push_back(0xFF);
            remaining -= 0xFF;
        }
        rbsp.push_back(static_cast<uint8_t>(remaining));
        rbsp.insert(rbsp.end(), payload.begin(), payload.end());
        // rbsp_trailing_bits: the stop bit and padding to a byte.
        rbsp.push_back(0x80);

        std::vector<uint8_t> nal;
        nal.reserve(rbsp.size() + 8);
        nal.insert(nal.end(), {0x00, 0x00, 0x00, 0x01});
        // nal_unit_type 39 (PREFIX_SEI_NUT) in bits 1..6, nuh_layer_id 0,
        // nuh_temporal_id_plus1 1.
        nal.push_back(static_cast<uint8_t>(39 << 1));
        nal.push_back(0x01);
        append_emulation_protected(nal, rbsp);
        return nal;
    }

    /// A slot for one submission, and the token that will retire it. The token rides in
    /// x265_picture::userData and comes back with the same picture on output.
    static uint64_t claim_slot(X265Session& s, HeldFrameData** out)
    {
        const uint64_t token = ++s.last_token;
        *out = &s.held[token];
        return token;
    }

    static void* token_to_user_data(uint64_t token)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(token));
    }

    /// Zero for a picture that carries no token of ours -- x265_picture_init clears
    /// userData, and tokens start at one, so "absent" and "valid" cannot be confused.
    static uint64_t token_from_user_data(const void* user_data)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user_data));
    }

    /// Regions to one float per block, in QP units and with our own sign, which is x265's
    /// too: "added on top of the decisions made by rateControl", so positive is worse.
    static void build_offsets(const X265Session& s, std::span<const Rect> regions,
        std::vector<float>& out)
    {
        const int block = s.offset_block;
        const int bw = (s.width + block - 1) / block;
        const int bh = (s.height + block - 1) / block;
        out.assign(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh), 0.0f);

        // Back to front, so the strongest region -- first in the list, §11 resolves overlap
        // by index -- is written last and survives.
        for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
            const Rect& r = *it;
            const int x0 = std::clamp(r.left / block, 0, bw - 1);
            const int x1 = std::clamp((r.right + block - 1) / block, 0, bw);
            const int y0 = std::clamp(r.top / block, 0, bh - 1);
            const int y1 = std::clamp((r.bottom + block - 1) / block, 0, bh);
            const float offset = static_cast<float>(r.qp_delta);
            for (int y = y0; y < y1; ++y) {
                float* row = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bw);
                std::fill(row + x0, row + x1, offset);
            }
        }
    }

    /// One call into x265. `pic_in` null asks for a delayed picture, which is how x265
    /// spells draining -- and once begun, every later call must pass null too.
    static Status encode_one(X265Session& s, x265_picture* pic_in)
    {
        x265_nal* nals = nullptr;
        uint32_t count = 0;
        x265_picture pic_out;
        x265_picture_init(&s.param, &pic_out);

        // 1 means a picture came out, 0 means the pipeline is still filling or is empty.
        // Not a byte count -- that is x264's convention, and reading it that way here would
        // make every output a one-byte packet.
        const int got = x265_encoder_encode(s.enc, &nals, &count, pic_in, &pic_out);
        if (got < 0) {
            return Status::BackendError("x265_encoder_encode failed");
        }
        // The picture is out, so x265 is finished with what came in with it: analysis has
        // read the offsets, and the SEI is ours to write into this very access unit. The
        // slot is taken out of the map here -- including for a picture that emerged
        // carrying no NALs, which is still a picture x265 has handed back -- and its SEI
        // bytes are moved out to be written below.
        std::vector<uint8_t> sei;
        if (got > 0) {
            const auto it = s.held.find(token_from_user_data(pic_out.userData));
            if (it != s.held.end()) {
                sei = std::move(it->second.sei);
                s.held.erase(it);
            }
        }
        if (got == 0) {
            if (pic_in == nullptr) {
                s.flush_done = true;
            }
            return Status::Ok();
        }
        if (count == 0) {
            return Status::Ok();
        }

        std::size_t total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            total += nals[i].sizeBytes;
        }

        // Copied NAL by NAL rather than in one go, because our SEI has to land in a
        // specific place. An access unit carries its parameter sets first and its prefix
        // SEI after them, before the first VCL unit -- putting SEI in front of the VPS
        // would be a non-conforming stream. HEVC VCL types are 0..31, non-VCL 32..63, and
        // x265 tells us the type of each NAL, so "before the first VCL" is exact rather
        // than guessed.
        s.payload.clear();
        s.payload.reserve(total + sei.size() + 16);
        bool sei_written = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!sei_written && !sei.empty() && nals[i].type < 32) {
                const std::vector<uint8_t> nal = prefix_sei_nal(sei);
                s.payload.insert(s.payload.end(), nal.begin(), nal.end());
                sei_written = true;
            }
            s.payload.insert(s.payload.end(), nals[i].payload,
                nals[i].payload + nals[i].sizeBytes);
        }
        s.pts = pic_out.pts;
        s.dts = pic_out.dts;
        s.key = pic_out.sliceType == X265_TYPE_IDR || pic_out.sliceType == X265_TYPE_I;
        s.has_packet = true;
        return Status::Ok();
    }
};

} // namespace

const IEncoderBackend& x265_direct_backend()
{
    static const X265Backend instance;
    return instance;
}

namespace {

const IEncoderBackend* acquire_x265_backend() noexcept
{
    return &x265_direct_backend();
}

/// One description for both delivery paths -- see the x264 component and §26.4.
constexpr SveBackendPluginDescriptorV1 kDescriptor{
    .abi_version = kBackendPluginAbiVersion,
    .descriptor_size = sizeof(SveBackendPluginDescriptorV1),
    .backend_id = static_cast<int32_t>(Backend::X265Direct),
    .acquire = &acquire_x265_backend,
};

} // namespace

/// §26.4's static registration for this component. See RegisterX264Backend.
Status RegisterX265Backend(BackendRegistry& registry) noexcept
{
    return registry.Register(kDescriptor);
}

#ifdef SVE_BACKEND_AS_PLUGIN
/// §26.4's dynamic delivery: one exported symbol, and it returns the very descriptor the
/// static registration above hands the registry. Compiled only into the module build,
/// because §26.4 fixes the symbol's name -- two static components carrying it would put two
/// definitions of one name on a deployment's link line.
///
/// extern "C" for the linkage, not for the types: what comes back is still the C++
/// descriptor, and noexcept is on the declaration because an exception leaving here would
/// unwind through a frame in the core that cannot see this module's types.
extern "C" SVE_BACKEND_EXPORT
const SveBackendPluginDescriptorV1* sve_backend_plugin_v1() noexcept
{
    return &kDescriptor;
}
#endif


} // namespace sve
