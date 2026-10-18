#include "engine/common/engine_common.h"
#include "ta_roienc/engine_catalog.h"

#include "engine/common/av_picture_store.h"
#include "engine/common/av_pixel_layout.h"
#include "coding_id.h"
#include "owned_layer_store.h"

#include <algorithm>
#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace ta_roienc {

LibavRun& libav(EngineRun& session)
{
    return static_cast<LibavRun&>(session);
}

namespace backend_common {

TenBitAbility query_ten_bit(const char* engine_name)
{
    TenBitAbility out;
    const AVCodec* codec = avcodec_find_encoder_by_name(engine_name);
    if (codec == nullptr) {
        return out;
    }
    // A context is needed because in this libavcodec the answer can depend on one -- a
    // hardware encoder reports what its device can do, not what the codec can in principle.
    AVCodecContext* probe = avcodec_alloc_context3(codec);
    if (probe == nullptr) {
        return out;
    }
    const void* configs = nullptr;
    int count = 0;
    const int err = avcodec_get_supported_config(
        probe, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count);
    if (err >= 0 && configs != nullptr) {
        const auto* formats = static_cast<const AVPixelFormat*>(configs);
        for (int i = 0; i < count; ++i) {
            const AVPixelFormat f = formats[i];
            if (f == AV_PIX_FMT_P010) {
                out.main10 = true;
                out.p010_input = true;
                out.encode_format = f;
                break;
            }
            if (f == AV_PIX_FMT_YUV420P10LE && !out.main10) {
                // Kept looking: P010 is preferred when both are offered, because taking it
                // directly is one conversion fewer.
                out.main10 = true;
                out.encode_format = f;
            }
        }
    }
    avcodec_free_context(&probe);
    return out;
}


/// The profile in libavcodec's numbering. Auto is left unset, so the encoder picks from the
/// pixel format -- which is what Auto means.
int av_profile_of(CodingProfile profile)
{
    switch (profile) {
    case CodingProfile::H264Baseline: return AV_PROFILE_H264_BASELINE;
    case CodingProfile::H264Main:     return AV_PROFILE_H264_MAIN;
    case CodingProfile::H264High:     return AV_PROFILE_H264_HIGH;
    case CodingProfile::H265Main:     return AV_PROFILE_HEVC_MAIN;
    case CodingProfile::H265Main10:   return AV_PROFILE_HEVC_MAIN_10;
    case CodingProfile::Auto:         break;
    }
    return AV_PROFILE_UNKNOWN;
}

void apply_shared(AVCodecContext* enc, const EngineSetup& p)
{
    if (const int profile = av_profile_of(p.profile); profile != AV_PROFILE_UNKNOWN) {
        enc->profile = profile;
    }
    const AVRational fps{p.input.frame_rate.num, std::max(p.input.frame_rate.den, 1)};
    enc->width = p.input.width;
    enc->height = p.input.height;
    enc->time_base = p.input.time_base.den > 0
        ? AVRational{p.input.time_base.num, p.input.time_base.den}
        : av_inv_q(fps);
    enc->framerate = fps;
    // Two seconds' worth when nothing was asked for, which is what a player expects to
    // wait before it can start.
    const int gop_auto = std::max(fps.num / std::max(fps.den, 1), 1) * 2;
    enc->gop_size = p.gop > 0 ? p.gop : gop_auto;
    enc->max_b_frames = 0;
}

bool open_hw_device(roiav::ByteBlock& hw, AVHWDeviceType type, const char* device)
{
    av_buffer_unref(&hw.p);
    return av_hwdevice_ctx_create(&hw.p, type, device, nullptr, 0) >= 0;
}

bool init_hw_frames(roiav::ByteBlock& device, roiav::ByteBlock& frames, AVPixelFormat hw_format,
    AVPixelFormat sw_format, int width, int height, int pool_size)
{
    av_buffer_unref(&frames.p);
    frames.p = av_hwframe_ctx_alloc(device.p);
    if (!frames.p) {
        return false;
    }
    auto* ctx = reinterpret_cast<AVHWFramesContext*>(frames.p->data);
    ctx->format = hw_format;
    ctx->sw_format = sw_format;
    ctx->width = width;
    ctx->height = height;
    ctx->initial_pool_size = pool_size;
    if (av_hwframe_ctx_init(frames.p) < 0) {
        av_buffer_unref(&frames.p);
        return false;
    }
    return true;
}

void attach_roi(AVFrame* frame, std::span<const Area> regions)
{
    av_frame_remove_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (regions.empty()) {
        return;
    }
    const int bytes =
        static_cast<int>(sizeof(AVRegionOfInterest) * regions.size());
    AVFrameSideData* sd =
        av_frame_new_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST, bytes);
    if (!sd) {
        roiav::fail("av_frame_new_side_data ROI");
    }
    auto* roi = reinterpret_cast<AVRegionOfInterest*>(sd->data);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        roi[i].self_size = sizeof(AVRegionOfInterest);
        roi[i].top = regions[i].top;
        roi[i].bottom = regions[i].bottom;
        roi[i].left = regions[i].left;
        roi[i].right = regions[i].right;
        const int qp = std::clamp(regions[i].qp_delta, kMinQpDelta, kMaxQpDelta);
        roi[i].qoffset = AVRational{qp, kMaxQp};
    }
}

void attach_sei_unregistered(AVFrame* frame, std::span<const uint8_t> uuid_and_payload)
{
    av_frame_remove_side_data(frame, AV_FRAME_DATA_SEI_UNREGISTERED);
    if (uuid_and_payload.empty()) {
        return;
    }
    AVFrameSideData* sd = av_frame_new_side_data(
        frame, AV_FRAME_DATA_SEI_UNREGISTERED, static_cast<int>(uuid_and_payload.size()));
    if (!sd) {
        roiav::fail("av_frame_new_side_data SEI");
    }
    std::memcpy(sd->data, uuid_and_payload.data(), uuid_and_payload.size());
}

} // namespace backend_common

namespace {

/// The buffer as an AVFrame, without copying when that is possible.
///
/// Two shapes. A buffer this library allocated hands back the AVFrame behind it, which is
/// what makes a factory surface cost nothing to submit. Anything else -- a LayerStore over
/// the caller's own planes, or an IPictureStore they implemented themselves -- is read
/// through the interface and wrapped, which costs no copy either as long as the format
/// matches. That second path is the reason the interface is worth having over a pointer.
AVFrame* as_avframe(LibavRun& s, IPictureStore& buffer)
{
    if (auto* ours = dynamic_cast<AvPictureStore*>(&buffer)) {
        return ours->frame();
    }
    const LayerView first = buffer.plane(0);
    if (first.empty() || to_av_pix_fmt(buffer.format()) == AV_PIX_FMT_NONE) {
        return nullptr;
    }
    AVFrame* f = s.wrapper.p;
    av_frame_unref(f);
    f->format = to_av_pix_fmt(buffer.format());
    f->width = buffer.width();
    f->height = buffer.height();
    for (int32_t i = 0; i < 4; ++i) {
        const LayerView view = buffer.plane(i);
        f->data[i] = view.bytes.empty() ? nullptr : view.bytes.data();
        f->linesize[i] = view.stride;
    }
    // A frame whose buf[] is empty is not reference-counted, and av_frame_ref on one of
    // those -- which is what avcodec_send_frame does with every frame it is given -- falls
    // back to av_frame_get_buffer plus av_frame_copy. That is a full copy of the picture
    // per frame, several megabytes at 1080p, and it is invisible from here: the call
    // succeeds either way.
    //
    // So when the pixels come with an owner, hand the owner over too. buf[0] is a
    // zero-length buffer that carries nothing but that handle, which is enough to make the
    // frame reference-counted: av_frame_ref then copies pointers and takes a reference,
    // and the caller's pixels stay alive for exactly as long as libavcodec holds them.
    // WrapPicture's promise of "without copying pixels" is this line.
    //
    // A buffer without an owner -- LayerStore, or an IPictureStore the caller implemented
    // -- deliberately does not get this. It is a borrowed view with no lifetime past the
    // submit call, so the copy is what makes it safe.
    if (auto* owned = dynamic_cast<OwnedLayerStore*>(&buffer)) {
        // The raw new/delete pair is the one place in this library that has them, and it
        // is not an oversight: av_buffer_create takes a void* opaque and a C function
        // pointer, so there is nothing for a smart pointer to be handed across. Both ends
        // are visible here, and av_buffer_create failing is the only path between them.
        auto* held = new std::shared_ptr<const void>(owned->owner());
        AVBufferRef* keepalive = av_buffer_create(nullptr, 0,
            [](void* opaque, uint8_t*) {
                delete static_cast<std::shared_ptr<const void>*>(opaque);
            },
            held, AV_BUFFER_FLAG_READONLY);
        if (keepalive == nullptr) {
            delete held;
            return nullptr;
        }
        f->buf[0] = keepalive;
    }
    return f;
}

/// Converts to what the encoder wants, or returns the frame untouched when it already is.
/// The untouched case is the one worth protecting: it is the difference between a copy per
/// frame and none, and it is what a frame from the backend's own factory always hits.
AVFrame* to_encoder_format(LibavRun& s, AVFrame* src)
{
    const bool same = src->format == s.sw_pix && src->width == s.enc.p->width
        && src->height == s.enc.p->height;
    if (same) {
        return src;
    }

    if (!s.sw_frame_ready) {
        s.sw_frame.p->format = s.sw_pix;
        s.sw_frame.p->width = s.enc.p->width;
        s.sw_frame.p->height = s.enc.p->height;
        roiav::check(av_frame_get_buffer(s.sw_frame.p, 32), "sw frame buffer");
        s.sw_frame_ready = true;
    }

    if (!s.sws.p || src->width != s.sws_src_w || src->height != s.sws_src_h
        || src->format != s.sws_src_fmt) {
        s.sws.reset();
        s.sws.p = sws_getContext(src->width, src->height,
            static_cast<AVPixelFormat>(src->format), s.enc.p->width, s.enc.p->height,
            s.sw_pix, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!s.sws.p) {
            roiav::fail("sws_getContext");
        }
        s.sws_src_w = src->width;
        s.sws_src_h = src->height;
        s.sws_src_fmt = src->format;
    }

    roiav::check(av_frame_make_writable(s.sw_frame.p), "make writable");
    sws_scale(s.sws.p, src->data, src->linesize, 0, src->height, s.sw_frame.p->data,
        s.sw_frame.p->linesize);
    ++s.frames_converted;
    return s.sw_frame.p;
}

bool is_hw_frame(const AVFrame* f)
{
    return f->hw_frames_ctx != nullptr;
}

} // namespace

Fallible<TranscoderPtr> LibavEngine::Configure(const EngineSetup& config) const
{
    const char* name = codec_name(config.codec);
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec && (kind() == Engine::X264 || kind() == Engine::X265)) {
        // A build without libx264/libx265 may still have some other encoder for the codec;
        // better that than refusing to run at all.
        codec = avcodec_find_encoder(av_id_from_codec(config.codec));
    }
    if (!codec) {
        return Verdict::DeviceUnavailable(std::string(name) + ": not in this FFmpeg build");
    }

    auto session = std::make_unique<LibavRun>();
    session->kind = kind();
    session->enc.p = avcodec_alloc_context3(codec);
    if (!session->enc.p) {
        return Verdict::Error("could not allocate an encoder context");
    }
    backend_common::apply_shared(session->enc.p, config);

    try {
        const Verdict s = configure_context(*session, config);
        if (!s) {
            return s;
        }
    } catch (const std::exception& e) {
        return Verdict::Error(e.what());
    }

    if (avcodec_open2(session->enc.p, codec, nullptr) < 0) {
        return Verdict::DeviceUnavailable(std::string(name) + ": would not open");
    }
    return TranscoderPtr(session.release());
}

Fallible<PictureStorePtr> LibavEngine::CreatePicture(
    EngineRun& session, MemoryKind memory) const
{
    LibavRun& s = libav(session);
    const bool hardware = s.enc.p->hw_frames_ctx != nullptr;
    // The device memory this session can hand out, if any. Asking a QSV encoder for VAAPI
    // surfaces is a different request from asking it for its own, even though both reach
    // the same silicon.
    const MemoryKind native =
        s.kind == Engine::IntelOneVpl ? MemoryKind::ONEVPL : MemoryKind::VAAPI;

    auto buffer = std::make_unique<AvPictureStore>(nullptr);

    // A caller who asked for system memory gets system memory, on every backend, even when
    // the session is a hardware one. Honouring the request costs an upload at submit, which
    // is counted in Outcome::frames_uploaded and therefore visible, and is what the caller
    // chose.
    if (memory == MemoryKind::CPU) {
        roiav::Picture f;
        f.p->format = s.sw_pix;
        f.p->width = s.enc.p->width;
        f.p->height = s.enc.p->height;
        if (av_frame_get_buffer(f.p, 32) < 0) {
            return Verdict::Error("could not allocate a frame");
        }
        AVFrame* raw = f.p;
        f.p = nullptr;
        buffer->adopt(raw, nullptr, MemoryKind::CPU);
        return PictureStorePtr(buffer.release());
    }

    if (!hardware || memory != native) {
        return Verdict::Unsupported(std::string("this encoder has no ")
            + memory_kind_name(memory) + " memory to hand out");
    }

    // Device memory: a surface out of the encoder's own pool, plus a CPU-writable view of
    // it. The view is what the caller fills; commit() drops it, and dropping it is what
    // makes the writes visible to the encoder. Nothing is copied on submit.
    roiav::Picture hw;
    if (av_hwframe_get_buffer(s.enc.p->hw_frames_ctx, hw.p, 0) < 0) {
        return Verdict::Error("could not take a surface from the encoder's pool");
    }
    roiav::Picture mapped;
    if (av_hwframe_map(mapped.p, hw.p, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE)
        < 0) {
        // Some drivers will not map a surface for writing. That is not a failure of the
        // request, it is the honest answer that this path costs a copy here -- and saying
        // so lets the caller fall back to CPU memory rather than believing in a zero-copy
        // that is not happening.
        return Verdict::Unsupported(
            "this driver will not map an encoder surface for CPU writes");
    }
    AVFrame* raw_hw = hw.p;
    AVFrame* raw_map = mapped.p;
    hw.p = nullptr;
    mapped.p = nullptr;
    buffer->adopt(raw_hw, raw_map, native);
    return PictureStorePtr(buffer.release());
}

Verdict LibavEngine::SubmitPicture(EngineRun& session, IPictureStore& frame,
    const EnginePictureControl& control) const
{
    LibavRun& s = libav(session);
    // A factory surface still mapped for CPU writing would encode whatever was in it
    // before the caller's writes reached the hardware. Refused rather than encoded: a
    // wrong picture is the one failure this library must not produce quietly.
    if (auto* ours = dynamic_cast<AvPictureStore*>(&frame); ours && !ours->committed()) {
        return Verdict::Error(
            "frame buffer was submitted without commit(); its writes are not visible "
            "to the encoder yet");
    }

    AVFrame* src = as_avframe(s, frame);
    if (!src) {
        return Verdict::Unsupported(std::string("cannot read a ")
            + pixel_layout_name(frame.format()) + " buffer in "
            + memory_kind_name(frame.memory()) + " memory");
    }

    try {
        AVFrame* send = src;
        // A frame that already lives in the encoder's own memory goes straight in. That is
        // the whole point of the factory: no conversion, no upload, no copy.
        if (!is_hw_frame(src)) {
            send = to_encoder_format(s, src);
        }
        send->pts = control.pts;
        // A frame handed straight through still carries the decoder's picture type.
        // Choosing I/P/B is the encoder's job, and honouring the decoder's answer would let
        // the input's GOP structure override the requested one.
        send->pict_type = AV_PICTURE_TYPE_NONE;
        backend_common::attach_roi(send, control.regions);
        backend_common::attach_sei_unregistered(send, control.sei_unregistered);

        if (s.hw_upload && !is_hw_frame(send)) {
            av_frame_unref(s.hw_frame.p);
            roiav::check(av_hwframe_get_buffer(s.enc.p->hw_frames_ctx, s.hw_frame.p, 0),
                "hwframe get");
            roiav::check(av_hwframe_transfer_data(s.hw_frame.p, send, 0), "hw upload");
            ++s.frames_uploaded;
            s.hw_frame.p->pts = control.pts;
            // Side data does not survive the transfer to hardware.
            backend_common::attach_roi(s.hw_frame.p, control.regions);
            backend_common::attach_sei_unregistered(s.hw_frame.p, control.sei_unregistered);
            send = s.hw_frame.p;
        }

        roiav::check(avcodec_send_frame(s.enc.p, send), "send frame");
    } catch (const std::exception& e) {
        return Verdict::Error(e.what());
    }
    return Verdict::Ok();
}

Fallible<CodingParams> LibavEngine::GetCodingParams(EngineRun& session) const
{
    const LibavRun& s = libav(session);
    if (s.enc.p == nullptr) {
        return Verdict::InvalidArgument("no encoder is open");
    }
    CodingParams out;
    out.codec = codec_from_av_id(s.enc.p->codec_id);
    out.width = s.enc.p->width;
    out.height = s.enc.p->height;
    out.time_base = Ratio{s.enc.p->time_base.num, s.enc.p->time_base.den};
    if (s.enc.p->extradata != nullptr && s.enc.p->extradata_size > 0) {
        out.extradata = AccessUnitData::Copy(std::span<const uint8_t>(
            s.enc.p->extradata, static_cast<std::size_t>(s.enc.p->extradata_size)));
    }
    return out;
}

EngineCounters LibavEngine::Statistics(const EngineRun& session) const
{
    const LibavRun& s = libav(const_cast<EngineRun&>(session));
    return EngineCounters{s.frames_converted, s.frames_uploaded};
}

NativeHandle LibavEngine::HardwareDevice(const EngineRun& session) const
{
    const LibavRun& s = libav(const_cast<EngineRun&>(session));
    if (!s.hw_device.p) {
        return {};  // x264 and x265 open none, and that is the whole answer
    }
    return NativeHandle{NativeHandleKind::FfmpegHardwareDevice, s.hw_device.p};
}

Fallible<AccessUnit> LibavEngine::ReceiveAccessUnit(EngineRun& session) const
{
    LibavRun& s = libav(session);
    av_packet_unref(s.pkt.p);
    const int r = avcodec_receive_packet(s.enc.p, s.pkt.p);
    if (r == AVERROR(EAGAIN)) {
        return Verdict::Again();
    }
    if (r == AVERROR_EOF) {
        return Verdict::EndOfStream();
    }
    if (r < 0) {
        return Verdict::Error("receive packet");
    }

    AccessUnit out;
    out.payload = AccessUnitData::Copy(std::span<const uint8_t>(
        s.pkt.p->data, static_cast<std::size_t>(s.pkt.p->size)));
    out.pts = s.pkt.p->pts;
    out.dts = s.pkt.p->dts;
    out.duration = s.pkt.p->duration;
    out.key_frame = (s.pkt.p->flags & AV_PKT_FLAG_KEY) != 0;
    out.codec = codec_from_av_id(s.enc.p->codec_id);
    out.time_base = Ratio{s.enc.p->time_base.num, s.enc.p->time_base.den};
    return out;
}

Verdict LibavEngine::Flush(EngineRun& session) const
{
    LibavRun& s = libav(session);
    const int r = avcodec_send_frame(s.enc.p, nullptr);
    if (r < 0 && r != AVERROR_EOF) {
        return Verdict::Error("flush encoder");
    }
    return Verdict::Ok();
}

const IEngine& onevpl_backend();
const IEngine& vaapi_backend();
const IEngine& x264_backend();
const IEngine& x265_backend();

/// The libavcodec-backed group, registered together because that is how they are built:
/// they share LibavEngine and one CMake option.
Verdict RegisterFfmpegEngines(EngineCatalog& registry) noexcept
{
    for (const IEngine* backend :
         {&x264_backend(), &x265_backend(), &onevpl_backend(), &vaapi_backend()}) {
        if (const Verdict s = registry.Register(*backend); !s.ok()) {
            return s;
        }
    }
    return Verdict::Ok();
}

} // namespace ta_roienc
