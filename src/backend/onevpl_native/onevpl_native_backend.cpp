// Intel oneVPL through its own API -- TZ v4.2 §5.2.
//
// The first backend here that reaches vendor silicon without libavcodec. §5.5 puts it in
// the dependency graph as sve_backend_onevpl -> Intel oneVPL, and that is the whole point:
// the FFmpeg-mediated QSV path silently clamps ROI to four regions on H.264 (measured, see
// STATUS.md) and cannot express a per-block map at all, so what the hardware can do was
// never visible from where we stood.
//
// Three things §5.2 asks for by name, and where each is:
//
//   * "Для ROI использовать capability query" -- GetCapabilities asks the dispatcher what
//     this implementation encodes rather than assuming, and the region limit comes from
//     intel_roi_limit.h, which is a measurement rather than a table.
//   * "отрицательная коррекция QP повышает качество" -- mfxExtEncoderROI::DeltaQP has the
//     same sign as ours, so nothing is inverted. Getting this backwards protects the
//     background and crushes the object, and both files look plausible.
//   * "библиотека не должна предполагать фиксированное число ROI" -- the list is clamped to
//     what the codec allows and the loss is reported, never silently dropped (§50).
//
// System memory in, and deliberately. VPL will take a VA surface and skip a copy, but the
// caller here has planes in system memory; handing VPL those and letting it upload is one
// copy, while allocating VA surfaces to copy into first is also one copy plus a device
// allocator to get wrong. The zero-copy path belongs with a caller who already has a
// surface, which is what CreateFrame is for and a later slice.

#include "adaptive/importance_map.h"
#include "backend/common/intel_roi_limit.h"
#include "backend/common/rate_control.h"
#include "sve/backend_plugin.h"
#include "sve/backend_registry.h"
#include "sve/encoder_backend.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <vpl/mfx.h>
#include <vpl/mfxvideo.h>

namespace sve {

namespace {

/// VPL's own spelling of a codec.
mfxU32 codec_id(Codec codec)
{
    return codec == Codec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
}

/// The one block size this backend paints at, for both codecs.
///
/// mfxExtMBQP's documented alignment rule is the 16x16 one -- "(width +15 /16) && (height
/// +15 /16)" -- and it is the only size the header states. HEVC additionally has BlockSize,
/// but the driver here echoes 0 back for every value asked (16, 32, 64 all measured), so
/// what it actually uses is not observable through the API and 16 is the size the alignment
/// rule guarantees. Finer than a CTU costs nothing: the driver averages down, it does not
/// read past the array.
constexpr int kMbqpBlock = 16;

/// The grid the Intel encoder works on. Surfaces are allocated to it and mfxExtEncoderROI
/// rectangles are snapped to it: an unaligned rectangle is not refused, it is rounded
/// somewhere we did not choose, and the measurement then answers a question nobody asked.
/// The same 16 as kMbqpBlock above, and for the same underlying reason -- it is the
/// macroblock -- but kept separate because one is a surface constraint and the other is the
/// size of a map cell, and the day HEVC's BlockSize becomes observable they part.
constexpr int32_t kRoiAlign = 16;

/// What mfxExtEncoderROI can physically carry: its ROI[] member is a fixed array of 256.
/// Distinct from intel_max_roi_regions(), which is how many the *driver* accepts and is far
/// smaller -- but the structure's own bound has to be honoured too, because exceeding it
/// writes past the end of a member rather than being refused.
constexpr std::size_t kMfxRoiCapacity = 256;

/// Rounded up to the encoder's grid.
mfxU16 align_up(int32_t value)
{
    return static_cast<mfxU16>((value + kRoiAlign - 1) & ~(kRoiAlign - 1));
}

/// Rounded down to it.
mfxU32 align_down(int32_t value)
{
    return static_cast<mfxU32>(std::max(0, value) & ~(kRoiAlign - 1));
}

/// The parameter set this backend opens an encoder with.
///
/// One function because the capability probe below has to ask about the same encoder the
/// caller will get. A probe that queries a differently shaped parameter set is a probe that
/// can answer yes to a question nobody asked -- and the answer it gives (qp_map) is one the
/// adaptive layer changes its own behaviour on.
/// CQP, or VBR when a target bitrate was asked for -- and the choice is the same one
/// GetCapabilities answered with, because both read the same target. See CapabilityQuery.
void fill_encode_params(mfxVideoParam& p, Codec codec, int32_t width, int32_t height,
    Rational frame_rate, int crf, int gop, uint64_t target_bitrate)
{
    p.mfx.CodecId = codec_id(codec);
    p.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    // VDENC, and not for the power. On Intel this is the path where ROI is actually
    // implemented for HEVC: without it every region degrades the area it covers,
    // whatever the sign of its delta -- measured at 0.931 against 0.998 for an
    // untouched frame, and 0.881 with the sign flipped, so it was not a sign error
    // either. H.264 works both ways, which is what made this look codec-specific.
    //
    // The hint was in our own notes: STATUS.md records the libavcodec QSV path as
    // "CQP + low_power", written down long before this backend existed.
    p.mfx.LowPower = MFX_CODINGOPTION_ON;
    p.mfx.GopRefDist = 1;   // no B-frames: keeps PTS in submission order
    p.mfx.GopPicSize = gop > 0 ? static_cast<mfxU16>(gop) : 0;
    p.mfx.IdrInterval = 0;
    p.mfx.NumSlice = 0;
    p.mfx.NumRefFrame = 0;
    p.mfx.EncodedOrder = 0;
    p.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    if (target_bitrate > 0) {
        // The hardware's own rate control, which is what §16's rolling criterion needs.
        // The block map is given up here and GetCapabilities has already said so.
        const VbvPlan vbv = vbv_plan_for(target_bitrate);
        p.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
        p.mfx.TargetKbps = static_cast<mfxU16>(std::min(vbv.bitrate_kbps, 65535));
        p.mfx.MaxKbps = static_cast<mfxU16>(std::min(vbv.max_kbps, 65535));
    } else {
        // CQP. It is what makes a per-region QP offset mean what it says: under a rate
        // control loop the offsets are advice the controller may spend elsewhere -- and the
        // block map this backend sends is stronger than advice, being absolute quantisers.
        p.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
        const auto qp = static_cast<mfxU16>(std::clamp(crf, kMinCodedQp, kMaxQp));
        p.mfx.QPI = qp;
        p.mfx.QPP = qp;
        p.mfx.QPB = qp;
    }

    mfxFrameInfo& fi = p.mfx.FrameInfo;
    fi.FourCC = MFX_FOURCC_NV12;
    fi.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    fi.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    fi.FrameRateExtN = static_cast<mfxU32>(std::max(frame_rate.num, 1));
    fi.FrameRateExtD = static_cast<mfxU32>(std::max(frame_rate.den, 1));
    fi.Width = align_up(width);
    fi.Height = align_up(height);
    fi.CropX = 0;
    fi.CropY = 0;
    fi.CropW = static_cast<mfxU16>(width);
    fi.CropH = static_cast<mfxU16>(height);
}

/// mfxExtCodingOption3, carrying the one field this backend sets on it.
mfxExtCodingOption3 coding_option3(mfxU16 enable_mbqp)
{
    mfxExtCodingOption3 co3{};
    co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    co3.Header.BufferSz = sizeof(co3);
    co3.EnableMBQP = enable_mbqp;
    return co3;
}

/// Does this implementation take a per-block QP map at all -- §2.2's block-level quality.
///
/// Query rather than a table: EnableMBQP is a tri-state the runtime corrects, so asking is
/// the only way that stays true across drivers. Measured here on iHD 25.2: it comes back ON
/// for both codecs and both LowPower modes, and it survives Init. BlockSize is not part of
/// the question -- the driver echoes 0 back whatever is asked for (16, 32 and 64 all
/// measured), so it is not observable and kMbqpBlock stands on the header's alignment rule.
///
/// Note what the answer is *not* asked about: without an mfxExtCodingOption3 attached at
/// all, GetVideoParam after Init reports EnableMBQP as OFF. So "does the driver support it"
/// and "did we ask for it" are the same question here, and the probe has to attach the
/// buffer to get an answer.
bool query_accepts_mbqp(mfxSession session, Codec codec)
{
    // A nominal clip. Whether the runtime implements MBQP is a property of the driver, not
    // of the resolution; 1280x720 is already 16-aligned, so no frame is described here that
    // the encoder would have to correct first.
    mfxVideoParam in{};
    fill_encode_params(in, codec, 1280, 720, Rational{25, 1}, 28, 0, 0);
    mfxExtCodingOption3 in_co3 = coding_option3(MFX_CODINGOPTION_ON);
    mfxExtBuffer* in_ext[1] = {&in_co3.Header};
    in.ExtParam = in_ext;
    in.NumExtParam = 1;

    // Its own buffer, and its own list of exactly the same shape. Query fills the output
    // set in place, and letting the two share one array would have it correcting the
    // request while reading it.
    mfxVideoParam out = in;
    mfxExtCodingOption3 out_co3 = coding_option3(MFX_CODINGOPTION_UNKNOWN);
    mfxExtBuffer* out_ext[1] = {&out_co3.Header};
    out.ExtParam = out_ext;
    out.NumExtParam = 1;

    if (MFXVideoENCODE_Query(session, &in, &out) < MFX_ERR_NONE) {
        return false;
    }
    return out_co3.EnableMBQP == MFX_CODINGOPTION_ON;
}

/// What this machine's oneVPL can do for one codec, asked once.
struct DeviceProbe {
    /// A hardware implementation opened for this codec.
    bool device = false;
    /// mfxExtCodingOption3::EnableMBQP survived Query -- the per-block QP map of §2.2.
    bool mbqp = false;
};

/// The dispatcher's answer to "is there hardware here that encodes this, and how well".
///
/// Asked rather than assumed, which is what §5.2 means by capability query and what §48
/// means when it allows a SKIP only with capability evidence. Creating a session is the
/// honest form of the question: an implementation that enumerates and then fails to open
/// is not an implementation this machine has.
///
/// MBQP is asked the same way, and it has to be asked rather than assumed for a reason the
/// adaptive layer depends on: effective_region_budget() drops the rectangle budget entirely
/// once qp_map is true, so a backend that claims the map and cannot deliver it would be
/// handed every region and would silently discard all but four (§50 forbids exactly that).
DeviceProbe run_probe(Codec codec)
{
    mfxLoader loader = MFXLoad();
    if (loader == nullptr) {
        return {};
    }
    DeviceProbe out;
    const auto set_u32 = [&](const char* name, mfxU32 value) {
        mfxConfig cfg = MFXCreateConfig(loader);
        mfxVariant v{};
        v.Version.Version = MFX_VARIANT_VERSION;
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = value;
        return MFXSetConfigFilterProperty(cfg,
            reinterpret_cast<const mfxU8*>(name), v) == MFX_ERR_NONE;
    };
    if (set_u32("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE)
        && set_u32("mfxImplDescription.mfxEncoderDescription.encoder.CodecID",
                   codec_id(codec))) {
        mfxSession session = nullptr;
        if (MFXCreateSession(loader, 0, &session) == MFX_ERR_NONE) {
            out.device = true;
            out.mbqp = query_accepts_mbqp(session, codec);
            MFXClose(session);
        }
    }
    MFXUnload(loader);
    return out;
}

/// Answered once per codec and kept.
///
/// Memoised rather than asked again: every call opens a dispatcher, enumerates
/// implementations and creates a session, and GetCapabilities is called for every encoder
/// this process creates and again by the §23 report. A GPU does not appear or leave during
/// a run, so the answer does not change; the magic-static initialisation is what makes this
/// safe to call from several threads.
const DeviceProbe& probe(Codec codec)
{
    if (codec == Codec::H264) {
        static const DeviceProbe h264 = run_probe(Codec::H264);
        return h264;
    }
    static const DeviceProbe h265 = run_probe(Codec::H265);
    return h265;
}

/// One opened encoder. Everything VPL needs to be handed back is owned here.
class VplSession final : public BackendSession {
public:
    ~VplSession() override
    {
        if (session != nullptr) {
            MFXVideoENCODE_Close(session);
            MFXClose(session);
        }
        if (loader != nullptr) {
            MFXUnload(loader);
        }
    }

    mfxLoader loader = nullptr;
    mfxSession session = nullptr;
    mfxVideoParam param{};
    Codec codec = Codec::H264;
    int width = 0;
    int height = 0;

    /// NV12 surfaces in system memory, one per frame VPL may hold at once -- and with each
    /// one, the ROI that frame was submitted with.
    ///
    /// The regions live on the surface rather than on the session, and that is a
    /// correctness requirement rather than tidiness. Per frame they go through
    /// mfxEncodeCtrl and not through mfxVideoParam: setting them on the video parameters
    /// looks plausible, compiles, and does nothing -- the first version of this file did
    /// exactly that and encoded every frame flat while reporting success.
    ///
    /// **VPL does not read them during EncodeFrameAsync.** The comment here used to say it
    /// did, on the strength of the API being documented as taking the control block for the
    /// duration of the call. It was wrong, and the experiment that settled it took five
    /// lines: overwrite the map with QP 51 the instant EncodeFrameAsync returns, and the
    /// output collapses -- SSIM in the protected zone 0.870 against 0.999, the file 41%
    /// smaller. So the buffers are read after the call, and one set of them shared by every
    /// frame is one frame overwriting a map another frame has not been encoded with yet.
    ///
    /// It never showed, because it cannot show on a static ROI: every frame's map is
    /// identical, so overwriting it changes nothing. A moving object is what it breaks, and
    /// that is the case the library exists for.
    ///
    /// The lifetime is the surface's own, and it is the runtime's signal rather than our
    /// guess: VPL clears Data.Locked when it has finished with a surface, and it cannot be
    /// finished with the frame while still to read the regions it was submitted with. This
    /// is the same shape as the x265 fix -- what is in flight is whatever the encoder has
    /// not handed back yet -- and it arrives here from the same lesson.
    struct Surface {
        std::vector<mfxU8> data;
        mfxFrameSurface1 surface{};
        bool in_use = false;

        mfxExtEncoderROI roi{};
        /// §2.2's block map: one quantiser per 16x16 block, raster order. Absolute values
        /// rather than deltas -- see set_qp_map. Sized once at allocate().
        mfxExtMBQP mbqp{};
        std::vector<int8_t> mbqp_cells;
        mfxExtBuffer* ext[1]{};
        mfxEncodeCtrl ctrl{};
    };
    std::vector<Surface> surfaces;

    mfxExtCodingOption3 co3{};
    mfxExtBuffer* init_ext[1]{};
    /// The deltas the quantisers are built from. Session-wide rather than per surface: it
    /// is consumed inside set_qp_map and nothing outside that function ever reads it.
    std::vector<int8_t> mbqp_scratch;
    /// Init confirmed EnableMBQP; without it the rectangles are all that can be sent.
    bool mbqp_ready = false;
    /// The quantiser the encoder opened at, which the map is written relative to.
    int base_qp = 28;

    /// Output. VPL fills the bitstream and hands back a sync point per frame in flight.
    struct Pending {
        std::vector<mfxU8> data;
        mfxBitstream bs{};
        mfxSyncPoint sync = nullptr;
    };
    std::vector<Pending> pending;
    std::size_t next_pending = 0;

    std::vector<uint8_t> payload;
    int64_t pts = 0;
    int64_t dts = 0;
    bool key = false;
    bool has_packet = false;
    bool draining = false;
    int64_t frames_converted = 0;
    int64_t frames_quantiser_clamped = 0;
};

VplSession& vpl(BackendSession& session)
{
    return static_cast<VplSession&>(session);
}

/// YUV420P to NV12, by hand.
///
/// VPL encodes NV12; the caller has planar YUV. Fifteen lines here rather than libswscale,
/// because pulling FFmpeg into this component would undo the one thing it exists to prove.
void to_nv12(const IFrameBuffer& src, mfxFrameSurface1& dst, int width, int height)
{
    const PlaneView y = const_cast<IFrameBuffer&>(src).plane(0);
    const PlaneView u = const_cast<IFrameBuffer&>(src).plane(1);
    const PlaneView v = const_cast<IFrameBuffer&>(src).plane(2);
    const mfxU16 pitch = dst.Data.Pitch;

    for (int row = 0; row < height; ++row) {
        std::memcpy(dst.Data.Y + static_cast<std::size_t>(row) * pitch,
            y.bytes.data() + static_cast<std::size_t>(row) * y.stride,
            static_cast<std::size_t>(width));
    }
    const int half_w = (width + 1) / 2;
    const int half_h = (height + 1) / 2;
    for (int row = 0; row < half_h; ++row) {
        mfxU8* out = dst.Data.UV + static_cast<std::size_t>(row) * pitch;
        const uint8_t* up = u.bytes.data() + static_cast<std::size_t>(row) * u.stride;
        const uint8_t* vp = v.bytes.data() + static_cast<std::size_t>(row) * v.stride;
        for (int col = 0; col < half_w; ++col) {
            out[col * 2] = up[col];
            out[col * 2 + 1] = vp[col];
        }
    }
}

class OneVplNativeBackend final : public IEncoderBackend {
public:
    Backend kind() const override { return Backend::IntelOneVplDirect; }

    EncoderCapabilities GetCapabilities(const CapabilityQuery& query) const override
    {
        const Codec codec = query.codec;
        // The one capability on this backend that depends on what is being asked for, and
        // the reason CapabilityQuery exists at all. With no target bitrate the encoder
        // opens CQP and mfxExtMBQP means what it says; with one it opens VBR, where a map
        // of absolute quantisers would fight the rate controller, and ROI goes back to
        // rectangles. Both answers are true, and which one is true depends on the caller.
        const bool cqp = query.target_bitrate == 0;
        EncoderCapabilities c;
        const DeviceProbe& device = probe(codec);
        if (!device.device) {
            // Empty rather than trimmed: §8.4's selection reads this to skip a backend
            // with nothing to offer, and a half-filled answer would let it through.
            return c;
        }
        (codec == Codec::H264 ? c.h264 : c.h265) = true;
        c.roi_rectangles = true;
        c.runtime_roi_update = true;
        // Measured on this silicon, not read from a table: four for H.264 and eight for
        // H.265. The FFmpeg path had to learn this the hard way -- see STATUS.md.
        //
        // Still declared with the map on, and deliberately: it is what the rectangle path
        // is clamped to if a driver turns out to have no MBQP, and the adaptive layer
        // already knows to ignore it while qp_map is true (effective_region_budget).
        c.max_roi_regions = intel_max_roi_regions(codec);
        // §2.2's block-level quality, through mfxExtMBQP. Asked of the driver rather than
        // assumed -- see query_accepts_mbqp -- because the whole rectangle budget is
        // dropped upstream on the strength of this answer.
        c.qp_map = cqp && device.mbqp;
        if (c.qp_map) {
            c.qp_map_block_width = kMbqpBlock;
            c.qp_map_block_height = kMbqpBlock;
        }
        // Still false, and now for a reason rather than for want of a backend. §7.2's
        // emphasis map is an importance hint rather than a quantiser offset, and the
        // pinned oneVPL headers have no expression of one: the word does not occur in
        // them, and mfxExtMBQP's three modes are all quantisers -- a QP, a QP delta, or a
        // per-block choice between those two. What this backend now delivers is the delta
        // map, which is qp_map. Reporting emphasis_map for it would be renaming a feature
        // rather than having it.
        c.emphasis_map = false;
        // The encoder's own bitrate control, which this backend does have -- and which is
        // the other half of the trade above. Asked for a target, it opens VBR and gives up
        // the block map; asked for none, it opens CQP and keeps it. Neither is a downgrade
        // of the other and neither is silent: the answer above says which one the caller
        // is about to get.
        //
        // This is the one mechanism on this hardware that can meet §16's rolling criterion.
        // §15's outer loop cannot -- measured, срез Q in docs/STATUS.md -- so refusing VBR
        // in order to keep the map would have cost §16 on Intel entirely.
        c.native_bitrate = true;
        return c;
    }

    Result<SessionPtr> Configure(const BackendConfig& config) const override
    {
        auto s = std::make_unique<VplSession>();
        s->codec = config.codec;
        s->width = config.input.width;
        s->height = config.input.height;

        s->loader = MFXLoad();
        if (s->loader == nullptr) {
            return Status::DeviceUnavailable("oneVPL dispatcher could not be loaded");
        }
        if (const Status st = require_hardware(*s, config.codec); !st.ok()) {
            return st;
        }
        if (MFXCreateSession(s->loader, 0, &s->session) != MFX_ERR_NONE) {
            return Status::DeviceUnavailable(
                "no oneVPL hardware implementation encodes this codec here");
        }

        mfxVideoParam& p = s->param;
        const uint64_t target = config.rate_control.target_bitrate;
        fill_encode_params(p, config.codec, config.input.width, config.input.height,
            config.input.frame_rate, config.crf, config.gop, target);

        // The per-block QP map of §2.2, asked for at Init because that is the only place
        // mfxExtCodingOption3::EnableMBQP can be set -- the runtime buffer of §26 below is
        // refused outright without it. Held in the session: VPL keeps the pointer in
        // mfxVideoParam, and Init reads it after this function's frame is gone.
        // Asked for only when the map is what this session is going to use. Under VBR it
        // would be asked for and then never sent, which is a promise made to the driver
        // that nothing keeps.
        s->co3 = coding_option3(target == 0 ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
        s->init_ext[0] = &s->co3.Header;
        p.ExtParam = s->init_ext;
        p.NumExtParam = 1;

        // What the encoder will actually do, which is not always what was asked. VPL
        // corrects a parameter set in place and says so; taking the corrected one is the
        // difference between encoding and finding out later that it refused.
        //
        // The corrected set gets its own extension buffer rather than sharing the one it is
        // correcting: Query reads the input and writes the output, and one buffer in both
        // roles would have it reading back what it had just decided.
        mfxVideoParam corrected = p;
        mfxExtCodingOption3 corrected_co3 = coding_option3(MFX_CODINGOPTION_UNKNOWN);
        mfxExtBuffer* corrected_ext[1] = {&corrected_co3.Header};
        corrected.ExtParam = corrected_ext;
        corrected.NumExtParam = 1;

        const mfxStatus query = MFXVideoENCODE_Query(s->session, &p, &corrected);
        if (query < MFX_ERR_NONE) {
            return Status::Unsupported(
                "oneVPL refused these encoder parameters (" + std::to_string(query) + ")");
        }
        s->co3 = corrected_co3;
        corrected.ExtParam = s->init_ext;
        p = corrected;

        const mfxStatus init = MFXVideoENCODE_Init(s->session, &p);
        if (init < MFX_ERR_NONE) {
            return Status::BackendError(
                "MFXVideoENCODE_Init failed (" + std::to_string(init) + ")");
        }

        // Asked of the open encoder rather than inferred from the corrected parameters,
        // because those are what VPL agreed to and this is what it did.
        //
        // A refusal here is fatal rather than a quiet fall back to rectangles, and that is
        // the point: GetCapabilities has already told the adaptive layer qp_map is true, and
        // effective_region_budget() dropped the rectangle budget on the strength of it. The
        // regions arriving from now on are a full block map, four of which this driver would
        // accept as rectangles. Encoding them anyway is the silent fallback §50 forbids;
        // failing sends §8.4 to the next backend in the chain, which is audible and correct.
        mfxVideoParam opened{};
        mfxExtCodingOption3 opened_co3 = coding_option3(MFX_CODINGOPTION_UNKNOWN);
        mfxExtBuffer* opened_ext[1] = {&opened_co3.Header};
        opened.ExtParam = opened_ext;
        opened.NumExtParam = 1;
        if (MFXVideoENCODE_GetVideoParam(s->session, &opened) < MFX_ERR_NONE) {
            return Status::BackendError("MFXVideoENCODE_GetVideoParam failed");
        }
        s->mbqp_ready = opened_co3.EnableMBQP == MFX_CODINGOPTION_ON;
        // The capability this session was chosen on, recomputed from the same inputs. A
        // disagreement means the answer given to the adaptive layer was wrong, and the
        // regions arriving from now on are sized for a promise this encoder cannot keep.
        const bool promised_map =
            GetCapabilities(CapabilityQuery{config.codec, target}).qp_map;
        if (!s->mbqp_ready && promised_map) {
            return Status::BackendError(
                "oneVPL accepted EnableMBQP when asked and dropped it when opened; "
                "the per-block QP map this backend reported is not there");
        }
        // From the open encoder rather than from what was asked, because the map is written
        // relative to the quantiser it will actually use and VPL may have corrected it.
        s->base_qp = opened.mfx.QPI > 0 ? opened.mfx.QPI
                                        : std::clamp(config.crf, kMinCodedQp, kMaxQp);

        if (const Status st = allocate(*s); !st.ok()) {
            return st;
        }
        return SessionPtr(s.release());
    }

    Result<FrameBufferPtr> CreateFrame(BackendSession&, MemoryType) const override
    {
        // A VA surface pool is the zero-copy path and its own slice; saying so is better
        // than handing back a system-memory buffer that pretends to be one.
        return Status::Unsupported("the native oneVPL backend has no frame pool yet");
    }

    Status SubmitFrame(BackendSession& session, IFrameBuffer& frame,
        const BackendFrameControl& control) const override
    {
        VplSession& s = vpl(session);
        if (s.draining) {
            return Status::InvalidArgument("SubmitFrame after Flush");
        }
        if (frame.format() != PixelFormat::YUV420P && frame.format() != PixelFormat::NV12) {
            return Status::Unsupported("the native oneVPL backend takes YUV420P or NV12");
        }

        VplSession::Surface* slot = free_surface(s);
        if (slot == nullptr) {
            // Every surface is still with the encoder. Again is the honest answer: the
            // caller drains and comes back, which is §24's backpressure.
            return Status::Again();
        }
        to_nv12(frame, slot->surface, s.width, s.height);
        ++s.frames_converted;
        slot->surface.Data.TimeStamp = static_cast<mfxU64>(control.pts);

        mfxEncodeCtrl* ctrl = set_regions(s, *slot, control.regions);

        VplSession::Pending& out = next_output(s);
        out.bs.DataOffset = 0;
        out.bs.DataLength = 0;

        mfxStatus st = MFX_ERR_NONE;
        for (;;) {
            st = MFXVideoENCODE_EncodeFrameAsync(s.session, ctrl, &slot->surface,
                &out.bs, &out.sync);
            if (st == MFX_WRN_DEVICE_BUSY) {
                continue;   // the documented spin; the device is working, not failing
            }
            break;
        }
        if (st == MFX_ERR_MORE_DATA) {
            // Taken in, nothing out yet. The surface is ours again immediately.
            slot->in_use = false;
            return Status::Ok();
        }
        if (st < MFX_ERR_NONE) {
            slot->in_use = false;
            return Status::BackendError(
                "MFXVideoENCODE_EncodeFrameAsync failed (" + std::to_string(st) + ")");
        }
        return Status::Ok();
    }

    Result<EncodedPacket> ReceivePacket(BackendSession& session) const override
    {
        VplSession& s = vpl(session);
        if (s.has_packet) {
            s.has_packet = false;
            EncodedPacket out;
            out.payload = PacketBuffer::Copy(s.payload);
            out.pts = s.pts;
            out.dts = s.dts;
            out.key_frame = s.key;
            out.codec = s.codec;
            // The unit the timestamps above are counted in. Left at its 1/1 default this
            // muxes a 25 fps clip as one frame a second -- the defect slice H3 fixed on the
            // other two direct backends, not repeated here.
            out.time_base = Rational{static_cast<int32_t>(s.param.mfx.FrameInfo.FrameRateExtD),
                static_cast<int32_t>(s.param.mfx.FrameInfo.FrameRateExtN)};
            return out;
        }
        if (const Status st = pull(s); !st.ok()) {
            return st;
        }
        if (!s.has_packet) {
            return s.draining ? Status::EndOfStream() : Status::Again();
        }
        return ReceivePacket(session);
    }

    Status Flush(BackendSession& session) const override
    {
        VplSession& s = vpl(session);
        if (s.draining) {
            return Status::Ok();
        }
        s.draining = true;
        // Null input until the encoder says it has nothing left. Each call may hand back
        // one more frame, which pull() then syncs.
        for (;;) {
            VplSession::Pending& out = next_output(s);
            out.bs.DataOffset = 0;
            out.bs.DataLength = 0;
            mfxStatus st = MFX_ERR_NONE;
            for (;;) {
                st = MFXVideoENCODE_EncodeFrameAsync(s.session, nullptr, nullptr,
                    &out.bs, &out.sync);
                if (st == MFX_WRN_DEVICE_BUSY) {
                    continue;
                }
                break;
            }
            if (st == MFX_ERR_MORE_DATA || st < MFX_ERR_NONE) {
                break;
            }
        }
        return Status::Ok();
    }

    Result<CodecParameters> GetCodecParameters(BackendSession& session) const override
    {
        VplSession& s = vpl(session);
        CodecParameters out;
        out.codec = s.codec;
        out.width = s.width;
        out.height = s.height;
        out.time_base = Rational{static_cast<int32_t>(s.param.mfx.FrameInfo.FrameRateExtD),
            static_cast<int32_t>(s.param.mfx.FrameInfo.FrameRateExtN)};

        // The parameter sets, asked of the encoder rather than scraped out of the first
        // packet: §8.11 wants extradata a muxer can use before any frame has been encoded.
        //
        // HEVC needs three, and mfxExtCodingOptionSPSPPS carries two. The VPS comes in its
        // own mfxExtCodingOptionVPS, and leaving it out produces a file that muxes, plays
        // nowhere and reports "VPS 0 does not exist" -- 4.6 MB of HEVC that no decoder will
        // touch, from an encode that returned success at every step.
        mfxExtCodingOptionSPSPPS spspps{};
        spspps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
        spspps.Header.BufferSz = sizeof(spspps);
        std::vector<mfxU8> sps(1024);
        std::vector<mfxU8> pps(1024);
        spspps.SPSBuffer = sps.data();
        spspps.SPSBufSize = static_cast<mfxU16>(sps.size());
        spspps.PPSBuffer = pps.data();
        spspps.PPSBufSize = static_cast<mfxU16>(pps.size());

        mfxExtCodingOptionVPS vps{};
        vps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_VPS;
        vps.Header.BufferSz = sizeof(vps);
        std::vector<mfxU8> vps_data(1024);
        vps.VPSBuffer = vps_data.data();
        vps.VPSBufSize = static_cast<mfxU16>(vps_data.size());

        std::vector<mfxExtBuffer*> buffers{&spspps.Header};
        const bool hevc = s.codec == Codec::H265;
        if (hevc) {
            buffers.push_back(&vps.Header);
        }
        mfxVideoParam got = s.param;
        got.ExtParam = buffers.data();
        got.NumExtParam = static_cast<mfxU16>(buffers.size());
        if (MFXVideoENCODE_GetVideoParam(s.session, &got) == MFX_ERR_NONE) {
            std::vector<uint8_t> extradata;
            if (hevc) {
                extradata.insert(extradata.end(), vps_data.begin(),
                    vps_data.begin() + vps.VPSBufSize);
            }
            extradata.insert(extradata.end(), sps.begin(), sps.begin() + spspps.SPSBufSize);
            extradata.insert(extradata.end(), pps.begin(), pps.begin() + spspps.PPSBufSize);
            if (!extradata.empty()) {
                out.extradata = PacketBuffer::Copy(extradata);
            }
        }
        return out;
    }

    BackendStatistics Statistics(const BackendSession& session) const override
    {
        BackendStatistics stats;
        stats.frames_converted =
            static_cast<const VplSession&>(session).frames_converted;
        stats.frames_quantiser_clamped = static_cast<long>(
            static_cast<const VplSession&>(session).frames_quantiser_clamped);
        return stats;
    }

private:
    /// Narrows the dispatcher to a hardware implementation that encodes this codec.
    static Status require_hardware(VplSession& s, Codec codec)
    {
        const auto want = [&](const char* name, mfxU32 value) {
            mfxConfig cfg = MFXCreateConfig(s.loader);
            mfxVariant v{};
            v.Version.Version = MFX_VARIANT_VERSION;
            v.Type = MFX_VARIANT_TYPE_U32;
            v.Data.U32 = value;
            return MFXSetConfigFilterProperty(cfg,
                reinterpret_cast<const mfxU8*>(name), v) == MFX_ERR_NONE;
        };
        if (!want("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE)
            || !want("mfxImplDescription.mfxEncoderDescription.encoder.CodecID",
                     codec_id(codec))) {
            return Status::DeviceUnavailable("oneVPL would not take the implementation filter");
        }
        return Status::Ok();
    }

    /// Surfaces and output buffers, sized from what the encoder says it needs.
    static Status allocate(VplSession& s)
    {
        mfxFrameAllocRequest request{};
        if (MFXVideoENCODE_QueryIOSurf(s.session, &s.param, &request) < MFX_ERR_NONE) {
            return Status::BackendError("MFXVideoENCODE_QueryIOSurf failed");
        }
        // Its own number, plus room to keep feeding while it holds some. Asked rather than
        // guessed: the ring that was guessed on the x265 side is why that is worth saying.
        const int count = std::max<int>(request.NumFrameSuggested, 1) + 4;
        const mfxU16 width = s.param.mfx.FrameInfo.Width;
        const mfxU16 height = s.param.mfx.FrameInfo.Height;
        const std::size_t plane = static_cast<std::size_t>(width) * height;

        const auto cells = static_cast<std::size_t>(qp_map_cols(s.width, kMbqpBlock))
            * static_cast<std::size_t>(qp_map_rows(s.height, kMbqpBlock));

        s.surfaces.resize(static_cast<std::size_t>(count));
        for (VplSession::Surface& surface : s.surfaces) {
            surface.data.assign(plane * 3 / 2, 0);
            surface.surface = {};
            surface.surface.Info = s.param.mfx.FrameInfo;
            surface.surface.Data.Y = surface.data.data();
            surface.surface.Data.UV = surface.data.data() + plane;
            surface.surface.Data.Pitch = width;
            // One map per frame in flight, not one per encoder: see the comment on Surface.
            if (s.mbqp_ready) {
                surface.mbqp_cells.assign(cells, 0);
            }
        }
        s.mbqp_scratch.assign(cells, 0);

        // How big one packet may be, asked of the encoder. Sizing this from the frame's
        // pixel count is the obvious guess and it is wrong: a CQP keyframe can exceed it,
        // and EncodeFrameAsync then answers MFX_ERR_NOT_ENOUGH_BUFFER, which reads as a
        // parameter problem and is not one.
        mfxVideoParam actual = s.param;
        std::size_t packet_bytes = plane;
        if (MFXVideoENCODE_GetVideoParam(s.session, &actual) == MFX_ERR_NONE
            && actual.mfx.BufferSizeInKB > 0) {
            const mfxU32 multiplier =
                actual.mfx.BRCParamMultiplier > 0 ? actual.mfx.BRCParamMultiplier : 1;
            packet_bytes = static_cast<std::size_t>(actual.mfx.BufferSizeInKB)
                * multiplier * 1000;
        }
        packet_bytes = std::max<std::size_t>(packet_bytes, plane);

        s.pending.resize(static_cast<std::size_t>(count));
        for (VplSession::Pending& out : s.pending) {
            out.data.assign(packet_bytes, 0);
            out.bs = {};
            out.bs.Data = out.data.data();
            out.bs.MaxLength = static_cast<mfxU32>(out.data.size());
        }
        return Status::Ok();
    }

    static VplSession::Surface* free_surface(VplSession& s)
    {
        for (VplSession::Surface& surface : s.surfaces) {
            // Locked is VPL's own answer to "am I still using this", which is the only
            // reliable one: the encoder holds a surface for as long as it references it.
            if (!surface.in_use && surface.surface.Data.Locked == 0) {
                surface.in_use = true;
                return &surface;
            }
        }
        return nullptr;
    }

    static VplSession::Pending& next_output(VplSession& s)
    {
        VplSession::Pending& out = s.pending[s.next_pending];
        s.next_pending = (s.next_pending + 1) % s.pending.size();
        return out;
    }

    /// Waits for the oldest frame the encoder owes us and turns it into a packet.
    static Status pull(VplSession& s)
    {
        for (VplSession::Pending& out : s.pending) {
            if (out.sync == nullptr) {
                continue;
            }
            const mfxStatus st = MFXVideoCORE_SyncOperation(s.session, out.sync, 60000);
            out.sync = nullptr;
            if (st < MFX_ERR_NONE) {
                return Status::BackendError(
                    "MFXVideoCORE_SyncOperation failed (" + std::to_string(st) + ")");
            }
            if (out.bs.DataLength == 0) {
                continue;
            }
            s.payload.assign(out.bs.Data + out.bs.DataOffset,
                out.bs.Data + out.bs.DataOffset + out.bs.DataLength);
            s.pts = static_cast<int64_t>(out.bs.TimeStamp);
            s.dts = static_cast<int64_t>(out.bs.DecodeTimeStamp);
            s.key = (out.bs.FrameType & MFX_FRAMETYPE_IDR) != 0
                 || (out.bs.FrameType & MFX_FRAMETYPE_I) != 0;
            s.has_packet = true;
            out.bs.DataLength = 0;
            out.bs.DataOffset = 0;
            // Surfaces the encoder has released are free again; VPL says so through Locked.
            for (VplSession::Surface& surface : s.surfaces) {
                if (surface.surface.Data.Locked == 0) {
                    surface.in_use = false;
                }
            }
            return Status::Ok();
        }
        return Status::Ok();
    }

    /// This frame's ROI, in whichever of the two forms the encoder opened with.
    ///
    /// The map when it is there, because it is the same information without the merge, and
    /// it is what §2.2 asks for. The rectangles otherwise. Never both: they are two ways of
    /// saying the same thing to one encoder, and the header does not say which would win.
    static mfxEncodeCtrl* set_regions(
        VplSession& s, VplSession::Surface& slot, std::span<const Rect> regions)
    {
        slot.roi = {};
        slot.mbqp = {};
        slot.ctrl = {};
        if (regions.empty()) {
            return nullptr;
        }
        return s.mbqp_ready ? set_qp_map(s, slot, regions) : set_roi(s, slot, regions);
    }

    /// The whole block grid, through mfxExtMBQP -- §2.2's block-level quality.
    ///
    /// This is what the rectangle list was always a lossy summary of. The adaptive layer
    /// paints a QP delta per coding block and then merges equal-delta blocks into
    /// rectangles, because rectangles are all mfxExtEncoderROI can carry -- four of them on
    /// H.264 here, eight on H.265. mfxExtMBQP carries the grid itself, so the merge is
    /// undone at the last moment and nothing is dropped: 3600 cells on a 1280x720 frame,
    /// against the four regions the same frame used to arrive as.
    ///
    /// Painted by the adaptive layer's own builder rather than by a second implementation
    /// here. Overlap resolves to the first rectangle, and a backend that resolved it the
    /// other way would protect the background over the object on exactly the frames where
    /// two regions meet. One rule, one place, already unit-tested.
    ///
    /// **Absolute quantisers, not deltas, and that is measured rather than chosen.**
    /// MFX_MBQP_MODE_QP_DELTA is documented as "QP[i] = BrcQP[i] + DeltaQP[i]", which is
    /// exactly what qp_delta means everywhere above this line -- and iHD 25.2 does not
    /// implement it. It accepts the mode, returns MFX_ERR_NONE, and reads the array as QP
    /// values anyway. The result was inverted and unmistakable -- HEVC, synth_plateonly,
    /// which asks only that the plate be improved and leaves the background alone: cells at
    /// 0, meaning "leave this block alone", became QP 0, so the untouched background came
    /// out at SSIM 0.999999 and the file grew 191%, while the plate, carrying -10, came out
    /// at SSIM 0.042. Every call succeeded. Writing base + delta into MFX_MBQP_MODE_QP_VALUE
    /// puts every one of those numbers back where it belongs -- and the CQP equalities the
    /// acceptance asserts come out exact, which is what says the cells land where painted.
    ///
    /// The base quantiser is knowable only because this backend opens CQP; that constraint
    /// is where native_bitrate = false comes from.
    static mfxEncodeCtrl* set_qp_map(
        VplSession& s, VplSession::Surface& slot, std::span<const Rect> regions)
    {
        const int cols = qp_map_cols(s.width, kMbqpBlock);
        const int rows = qp_map_rows(s.height, kMbqpBlock);
        const auto cells = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
        if (cells == 0 || slot.mbqp_cells.size() < cells
            || s.mbqp_scratch.size() < cells) {
            return set_roi(s, slot, regions);
        }
        // Reset to the frame's own quantiser rather than to zero: a cell no region covers
        // means "the encoder's own decision", and in a map of absolute values that is the
        // base QP. Zero would be lossless, which is the bug above in miniature.
        const auto base = static_cast<int8_t>(std::clamp(s.base_qp, kMinCodedQp, kMaxQp));
        std::fill(slot.mbqp_cells.begin(), slot.mbqp_cells.end(), base);
        // Painted as deltas into a scratch grid, then added to the base. Two passes over
        // 3600 cells rather than teaching the shared painter about a base it has no
        // business knowing -- the merge rule is what is worth sharing, not the arithmetic.
        std::fill(s.mbqp_scratch.begin(), s.mbqp_scratch.end(), int8_t{0});
        QpDeltaMapBuilder().paint(s.width, s.height, kMbqpBlock, regions, s.mbqp_scratch);
        bool clamped = false;
        for (std::size_t i = 0; i < cells; ++i) {
            const int wanted = s.base_qp + s.mbqp_scratch[i];
            const int given = std::clamp(wanted, kMinCodedQp, kMaxQp);
            clamped = clamped || given != wanted;
            slot.mbqp_cells[i] = static_cast<int8_t>(given);
        }
        // Counted per frame rather than per cell: what a caller can act on is "the scale
        // ran out", not how many blocks it ran out in. §50 -- a request that quietly did
        // less than it asked for has to be visible.
        if (clamped) {
            ++s.frames_quantiser_clamped;
        }

        slot.mbqp.Header.BufferId = MFX_EXTBUFF_MBQP;
        slot.mbqp.Header.BufferSz = sizeof(mfxExtMBQP);
        slot.mbqp.Mode = MFX_MBQP_MODE_QP_VALUE;
        slot.mbqp.BlockSize = static_cast<mfxU16>(kMbqpBlock);
        slot.mbqp.Pitch = static_cast<mfxU32>(cols);
        slot.mbqp.NumQPAlloc = static_cast<mfxU32>(cells);
        // The union's QP arm, matching the mode above. mfxU8 where the cells are int8_t:
        // every value written is 1..51, so the two spellings agree on the bytes, and a cast
        // between character types is the one aliasing C++ always allows.
        slot.mbqp.QP = reinterpret_cast<mfxU8*>(slot.mbqp_cells.data());

        slot.ext[0] = &slot.mbqp.Header;
        slot.ctrl.ExtParam = slot.ext;
        slot.ctrl.NumExtParam = 1;
        return &slot.ctrl;
    }

    /// The regions, clamped to what this codec's hardware accepts.
    ///
    /// §5.2 forbids assuming a fixed number, and §50 forbids losing any of them quietly.
    /// The list arrives strongest-first, so truncating from the end drops the least
    /// important -- which is the only truncation that can be defended.
    ///
    /// Reached only when the map is not available. It is kept rather than deleted because
    /// "this driver has no MBQP" is a machine we cannot see from here, not a case that
    /// cannot happen.
    static mfxEncodeCtrl* set_roi(
        VplSession& s, VplSession::Surface& slot, std::span<const Rect> regions)
    {
        slot.roi.Header.BufferId = MFX_EXTBUFF_ENCODER_ROI;
        slot.roi.Header.BufferSz = sizeof(mfxExtEncoderROI);
        // Without this the field below is not a QP delta at all. ROIMode defaults to
        // MFX_ROI_MODE_PRIORITY, and Priority shares its memory with DeltaQP through a
        // union -- range -3..3, and the header says "Bigger value produces better quality",
        // which is the opposite sense. The first version of this file left it unset and
        // the measurement came out exactly inverted: the protected zone lost quality
        // (0.9977 to 0.9921) while the background gained it (0.9966 to 0.99999) and the
        // file grew by 68%. Every call succeeded.
        slot.roi.ROIMode = MFX_ROI_MODE_QP_DELTA;
        // Overlap resolves to the lowest index -- the header is explicit: "If macroblock
        // belongs to several ROI, Priority from ROI with lowest index is used". Our list
        // arrives strongest-first, which is also how §11 resolves overlap, so the two
        // conventions agree and the object beats the background rectangle underneath it.
        const int limit = intel_max_roi_regions(s.codec);
        const std::size_t take = std::min<std::size_t>(regions.size(),
            std::min<std::size_t>(static_cast<std::size_t>(limit), kMfxRoiCapacity));
        slot.roi.NumROI = static_cast<mfxU16>(take);
        for (std::size_t i = 0; i < take; ++i) {
            const Rect& r = regions[i];
            // Aligned outwards to the grid the hardware works on -- see kRoiAlign.
            slot.roi.ROI[i].Left = align_down(r.left);
            slot.roi.ROI[i].Top = align_down(r.top);
            slot.roi.ROI[i].Right = align_up(std::max(0, r.right));
            slot.roi.ROI[i].Bottom = align_up(std::max(0, r.bottom));
            // Same sign as ours: §5.2 says negative raises quality, and so does qp_delta.
            slot.roi.ROI[i].DeltaQP = static_cast<mfxI16>(std::clamp(r.qp_delta, kMinQpDelta, kMaxQpDelta));
        }
        slot.ext[0] = &slot.roi.Header;
        slot.ctrl.ExtParam = slot.ext;
        slot.ctrl.NumExtParam = 1;
        return &slot.ctrl;
    }
};

} // namespace

const IEncoderBackend& onevpl_native_backend()
{
    static const OneVplNativeBackend instance;
    return instance;
}

namespace {

const IEncoderBackend* acquire_onevpl_backend() noexcept
{
    return &onevpl_native_backend();
}

/// One description for both delivery paths -- see the x264 component and §26.4.
constexpr SveBackendPluginDescriptorV1 kDescriptor{
    .abi_version = kBackendPluginAbiVersion,
    .descriptor_size = sizeof(SveBackendPluginDescriptorV1),
    .backend_id = static_cast<int32_t>(Backend::IntelOneVplDirect),
    .acquire = &acquire_onevpl_backend,
};

} // namespace

/// §26.4's static registration for this component.
Status RegisterOneVplBackend(BackendRegistry& registry) noexcept
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
