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
// The lifetime is undocumented, and that is the finding rather than an oversight to work
// around. x264 gives quant_offsets_free and sei_free; x265's x265_picture has a bare
// `float *quantOffsets` and an x265_sei with no callback at all, so the header says
// nothing about who owns either or for how long. Guessing "it copies during encode" is
// exactly the guess that cost a crash on the x264 side, where the header did say
// otherwise. So neither is freed early: both live in a per-session ring, and the ring is
// sized from the encoder's own numbers -- lookaheadDepth, bframes and frameNumThreads read
// back through x265_encoder_parameters after open, which is what x265 actually chose
// rather than what was asked for. That is correct whether x265 copies or holds, bounded in
// memory, and derived rather than picked.
//
// The return convention differs too: x264_encoder_encode returns a byte count, x265's
// returns 1 when a picture came out and 0 while the pipeline is filling. Reading it as a
// size would treat every output as a one-byte packet.

#include "backend/common/backend_common.h"
#include "preset.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <x265.h>
}

namespace sve {

namespace {

/// One frame's worth of the two things x265 is handed and never says it has finished with.
struct HeldFrameData {
    std::vector<float> offsets;
    std::vector<uint8_t> sei;
    x265_sei_payload payload{};
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

    /// In-flight frame data, kept until the ring wraps. See the header comment: x265 never
    /// says when it is done with these, so the only safe bound is one it cannot outlive.
    std::deque<HeldFrameData> held;
    std::size_t held_limit = 64;

    std::vector<uint8_t> payload;
    int64_t pts = 0;
    int64_t dts = 0;
    bool key = false;
    bool has_packet = false;
    bool draining = false;
};

X265Session& x265_session(BackendSession& session)
{
    return static_cast<X265Session&>(session);
}

class X265Backend final : public IEncoderBackend {
public:
    Backend kind() const override { return Backend::X265Direct; }

    EncoderCapabilities GetCapabilities(Codec codec) const override
    {
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
        // The HEVC target-bitrate path is the one decision Р5 refused on this stack, and
        // going direct does not by itself make it stable. Claimed when it is measured.
        c.native_bitrate = false;
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
        p.rc.rateControlMode = X265_RC_CRF;
        p.rc.rfConstant = static_cast<double>(config.crf);

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
        const int in_flight = effective.lookaheadDepth + effective.bframes
            + effective.frameNumThreads;
        session->held_limit = static_cast<std::size_t>(std::max(in_flight, 16)) + 8;

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

        HeldFrameData& held = take_slot(s);
        if (!control.regions.empty()) {
            build_offsets(s, control.regions, held.offsets);
            pic_in.quantOffsets = held.offsets.data();
        }
        if (!control.sei_unregistered.empty()) {
            held.sei.assign(control.sei_unregistered.begin(), control.sei_unregistered.end());
            held.payload.payload = held.sei.data();
            held.payload.payloadSize = static_cast<int>(held.sei.size());
            held.payload.payloadType = USER_DATA_UNREGISTERED;
            pic_in.userSEI.numPayloads = 1;
            pic_in.userSEI.payloads = &held.payload;
        }

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
            return out;
        }
        if (!s.draining) {
            return Status::Again();
        }
        const Status pulled = encode_one(s, nullptr);
        if (!pulled.ok()) {
            return pulled;
        }
        if (!s.has_packet) {
            return Status::EndOfStream();
        }
        return ReceivePacket(session);
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

    BackendStatistics Statistics(const BackendSession&) const override
    {
        // Nothing is converted here: anything but YUV420P is refused rather than converted
        // behind the caller's back.
        return BackendStatistics{};
    }

private:
    /// The next slot in the ring, recycling the oldest once the ring is full. By then the
    /// frame it belonged to is further behind than any lookahead x265 reported.
    static HeldFrameData& take_slot(X265Session& s)
    {
        s.held.emplace_back();
        while (s.held.size() > s.held_limit) {
            s.held.pop_front();
        }
        return s.held.back();
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
        if (got == 0 || count == 0) {
            return Status::Ok();
        }

        std::size_t total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            total += nals[i].sizeBytes;
        }
        // "the payloads of all output NALs are guaranteed to be sequential in memory", so
        // one copy from the first payload covers all of them.
        s.payload.assign(nals[0].payload, nals[0].payload + total);
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

} // namespace sve
