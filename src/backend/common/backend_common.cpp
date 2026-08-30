#include "backend/common/backend_common.h"

#include "backend/common/av_frame_buffer.h"
#include "codec_id.h"
#include "pixel_format.h"

#include <algorithm>
#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace sve {

// The deleters, compiled here on purpose: this is the library's side of the ABI, and so
// the library's allocator is the one that frees what it allocated. Inlining these into the
// caller is exactly what must not happen.
void SessionDeleter::operator()(BackendSession* session) const noexcept
{
    delete session;
}

void FrameBufferDeleter::operator()(IFrameBuffer* buffer) const noexcept
{
    delete buffer;
}

LibavSession& libav(BackendSession& session)
{
    return static_cast<LibavSession&>(session);
}

namespace backend_common {

void apply_shared(AVCodecContext* enc, const EncoderConfig& p)
{
    const AVRational fps{p.fps.num, std::max(p.fps.den, 1)};
    enc->width = p.width;
    enc->height = p.height;
    enc->time_base = p.time_base.den > 0
        ? AVRational{p.time_base.num, p.time_base.den}
        : av_inv_q(fps);
    enc->framerate = fps;
    // Two seconds' worth when nothing was asked for, which is what a player expects to
    // wait before it can start.
    const int gop_auto = std::max(fps.num / std::max(fps.den, 1), 1) * 2;
    enc->gop_size = p.gop > 0 ? p.gop : gop_auto;
    enc->max_b_frames = 0;
}

bool open_hw_device(roiav::Buffer& hw, AVHWDeviceType type, const char* device)
{
    av_buffer_unref(&hw.p);
    return av_hwdevice_ctx_create(&hw.p, type, device, nullptr, 0) >= 0;
}

bool init_hw_frames(roiav::Buffer& device, roiav::Buffer& frames, AVPixelFormat hw_format,
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

void attach_roi(AVFrame* frame, std::span<const Rect> regions)
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
        const int qp = std::clamp(regions[i].qp_delta, -51, 51);
        roi[i].qoffset = AVRational{qp, 51};
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
/// what makes a factory surface cost nothing to submit. Anything else -- a PlaneBuffer over
/// the caller's own planes, or an IFrameBuffer they implemented themselves -- is read
/// through the interface and wrapped, which costs no copy either as long as the format
/// matches. That second path is the reason the interface is worth having over a pointer.
AVFrame* as_avframe(LibavSession& s, IFrameBuffer& buffer)
{
    if (auto* ours = dynamic_cast<AvFrameBuffer*>(&buffer)) {
        return ours->frame();
    }
    const PlaneView first = buffer.plane(0);
    if (first.empty() || to_av_pix_fmt(buffer.format()) == AV_PIX_FMT_NONE) {
        return nullptr;
    }
    AVFrame* f = s.wrapper.p;
    av_frame_unref(f);
    f->format = to_av_pix_fmt(buffer.format());
    f->width = buffer.width();
    f->height = buffer.height();
    for (int32_t i = 0; i < 4; ++i) {
        const PlaneView view = buffer.plane(i);
        f->data[i] = view.bytes.empty() ? nullptr : view.bytes.data();
        f->linesize[i] = view.stride;
    }
    return f;
}

/// Converts to what the encoder wants, or returns the frame untouched when it already is.
/// The untouched case is the one worth protecting: it is the difference between a copy per
/// frame and none, and it is what a frame from the backend's own factory always hits.
AVFrame* to_encoder_format(LibavSession& s, AVFrame* src)
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

Result<SessionPtr> LibavBackend::Configure(const EncoderConfig& config) const
{
    const char* name = codec_name(config.codec);
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec && kind() == Encoder::Cpu) {
        // A build without libx264/libx265 may still have some other encoder for the codec;
        // better that than refusing to run at all.
        codec = avcodec_find_encoder(av_id_from_codec(config.codec));
    }
    if (!codec) {
        return Status::Unavailable(std::string(name) + ": not in this FFmpeg build");
    }

    auto session = std::make_unique<LibavSession>();
    session->kind = kind();
    session->enc.p = avcodec_alloc_context3(codec);
    if (!session->enc.p) {
        return Status::Error("could not allocate an encoder context");
    }
    backend_common::apply_shared(session->enc.p, config);

    try {
        const Status s = configure_context(*session, config);
        if (!s) {
            return s;
        }
    } catch (const std::exception& e) {
        return Status::Error(e.what());
    }

    if (avcodec_open2(session->enc.p, codec, nullptr) < 0) {
        return Status::Unavailable(std::string(name) + ": would not open");
    }
    return SessionPtr(session.release());
}

Result<FrameBufferPtr> LibavBackend::CreateFrame(
    BackendSession& session, MemoryType memory) const
{
    LibavSession& s = libav(session);
    const bool hardware = s.enc.p->hw_frames_ctx != nullptr;
    // The device memory this session can hand out, if any. Asking a QSV encoder for VAAPI
    // surfaces is a different request from asking it for its own, even though both reach
    // the same silicon.
    const MemoryType native =
        s.kind == Encoder::Qsv ? MemoryType::ONEVPL : MemoryType::VAAPI;

    auto buffer = std::make_unique<AvFrameBuffer>(nullptr);

    // A caller who asked for system memory gets system memory, on every backend.
    //
    // This used to ignore `memory` the moment the session was a hardware one and hand back
    // a mapped surface regardless. Nothing was reported: buffer->memory() answered ONEVPL
    // to a caller who had asked for CPU, so the only way to find out was to look at what
    // arrived. Honouring the request costs an upload at submit, which is counted in
    // Report::frames_uploaded and therefore visible, and is what the caller chose.
    if (memory == MemoryType::CPU) {
        roiav::Frame f;
        f.p->format = s.sw_pix;
        f.p->width = s.enc.p->width;
        f.p->height = s.enc.p->height;
        if (av_frame_get_buffer(f.p, 32) < 0) {
            return Status::Error("could not allocate a frame");
        }
        AVFrame* raw = f.p;
        f.p = nullptr;
        buffer->adopt(raw, nullptr, MemoryType::CPU);
        return FrameBufferPtr(buffer.release());
    }

    if (!hardware || memory != native) {
        return Status::Unsupported(std::string("this encoder has no ")
            + memory_type_name(memory) + " memory to hand out");
    }

    // Device memory: a surface out of the encoder's own pool, plus a CPU-writable view of
    // it. The view is what the caller fills; commit() drops it, and dropping it is what
    // makes the writes visible to the encoder. Nothing is copied on submit.
    roiav::Frame hw;
    if (av_hwframe_get_buffer(s.enc.p->hw_frames_ctx, hw.p, 0) < 0) {
        return Status::Error("could not take a surface from the encoder's pool");
    }
    roiav::Frame mapped;
    if (av_hwframe_map(mapped.p, hw.p, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE)
        < 0) {
        // Some drivers will not map a surface for writing. That is not a failure of the
        // request, it is the honest answer that this path costs a copy here -- and saying
        // so lets the caller fall back to CPU memory rather than believing in a zero-copy
        // that is not happening.
        return Status::Unsupported(
            "this driver will not map an encoder surface for CPU writes");
    }
    AVFrame* raw_hw = hw.p;
    AVFrame* raw_map = mapped.p;
    hw.p = nullptr;
    mapped.p = nullptr;
    buffer->adopt(raw_hw, raw_map, native);
    return FrameBufferPtr(buffer.release());
}

Status LibavBackend::SubmitFrame(BackendSession& session, IFrameBuffer& frame,
    const BackendFrameControl& control) const
{
    LibavSession& s = libav(session);
    // A factory surface still mapped for CPU writing would encode whatever was in it
    // before the caller's writes reached the hardware. Refused rather than encoded: a
    // wrong picture is the one failure this library must not produce quietly.
    if (auto* ours = dynamic_cast<AvFrameBuffer*>(&frame); ours && !ours->committed()) {
        return Status::Error(
            "frame buffer was submitted without commit(); its writes are not visible "
            "to the encoder yet");
    }

    AVFrame* src = as_avframe(s, frame);
    if (!src) {
        return Status::Unsupported(std::string("cannot read a ")
            + pixel_format_name(frame.format()) + " buffer in "
            + memory_type_name(frame.memory()) + " memory");
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
        return Status::Error(e.what());
    }
    return Status::Ok();
}

Result<EncodedPacket> LibavBackend::ReceivePacket(BackendSession& session) const
{
    LibavSession& s = libav(session);
    av_packet_unref(s.pkt.p);
    const int r = avcodec_receive_packet(s.enc.p, s.pkt.p);
    if (r == AVERROR(EAGAIN)) {
        return Status::Again();
    }
    if (r == AVERROR_EOF) {
        return Status::EndOfStream();
    }
    if (r < 0) {
        return Status::Error("receive packet");
    }

    EncodedPacket out;
    out.payload = PacketBuffer::Copy(std::span<const uint8_t>(
        s.pkt.p->data, static_cast<std::size_t>(s.pkt.p->size)));
    out.pts = s.pkt.p->pts;
    out.dts = s.pkt.p->dts;
    out.duration = s.pkt.p->duration;
    out.key_frame = (s.pkt.p->flags & AV_PKT_FLAG_KEY) != 0;
    out.codec = codec_from_av_id(s.enc.p->codec_id);
    out.time_base = Rational{s.enc.p->time_base.num, s.enc.p->time_base.den};
    return out;
}

Status LibavBackend::Flush(BackendSession& session) const
{
    LibavSession& s = libav(session);
    const int r = avcodec_send_frame(s.enc.p, nullptr);
    if (r < 0 && r != AVERROR_EOF) {
        return Status::Error("flush encoder");
    }
    return Status::Ok();
}

} // namespace sve
