#include "roi_encoder.h"

#include "backend/common/av_frame_buffer.h"
#include "backend/common/backend_common.h"
#include "packet_sink.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace sve {


using roiav::check;
using roiav::fail;

const char* RoiEncoder::label() const
{
    const bool h264 = m_codec == Codec::H264;
    switch (m_kind) {
    case Encoder::Qsv:
        return h264 ? "h264_qsv (Intel VPL mfxExtEncoderROI)"
                    : "hevc_qsv (Intel VPL mfxExtEncoderROI)";
    case Encoder::Vaapi:
        return h264 ? "h264_vaapi (Intel iHD VAEncROI)"
                    : "hevc_vaapi (Intel iHD VAEncROI)";
    case Encoder::Nvenc:
        return h264 ? "h264_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)"
                    : "hevc_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)";
    default:
        return h264 ? "libx264 (CPU fallback)" : "libx265 (CPU fallback)";
    }
}

namespace {

const char* backend_name(Encoder kind)
{
    switch (kind) {
    case Encoder::Qsv:   return "qsv";
    case Encoder::Vaapi: return "vaapi";
    case Encoder::Nvenc: return "nvenc";
    default:             return "cpu";
    }
}

// A throwaway encode of a single frame, for backends that ask for it. Ubuntu's older iHD
// opens the QSV encoder happily and then returns empty packets, and only an actual encode
// tells the two apart -- which is why IEncoderBackend::NeedsProbe exists rather than the
// caller guessing which backends lie.
//
// Written entirely against the interface: a grey frame from the backend's own factory,
// submitted and drained the way any caller would. It used to build an AVFrame by hand,
// which meant the one piece of code whose job is to find out whether a backend works could
// only ever have worked for libavcodec backends.
bool probe_one_frame(const IEncoderBackend& backend, BackendSession& session)
{
    auto made = backend.CreateFrame(session, MemoryType::CPU);
    if (!made) {
        return false;
    }
    IFrameBuffer* buffer = made.value().get();

    // Mid-grey: luma at 0, chroma at 128. Any content would do -- what is being tested is
    // whether a frame comes back out at all. The spans carry their own length, so there is
    // no height arithmetic here to get wrong.
    const PlaneView y = buffer->plane(0);
    std::fill(y.bytes.begin(), y.bytes.end(), uint8_t{0});
    const PlaneView uv = buffer->plane(1);
    std::fill(uv.bytes.begin(), uv.bytes.end(), uint8_t{128});
    buffer->commit();

    if (!backend.SubmitFrame(session, *buffer, BackendFrameControl{})) {
        return false;
    }
    auto pkt = backend.ReceivePacket(session);
    if (!pkt && pkt.status().code() == StatusCode::Again) {
        if (!backend.Flush(session)) {
            return false;
        }
        pkt = backend.ReceivePacket(session);
    }
    return pkt.ok();
}

} // namespace

void RoiEncoder::open(const EncoderConfig& config, PacketSink& sink)
{
    open_deferred(config);
    attach(sink);
}

void RoiEncoder::attach(PacketSink& sink)
{
    m_sink = &sink;
    m_sink->begin(session().enc.p);
}

EncoderCapabilities RoiEncoder::capabilities() const
{
    return m_backend ? m_backend->GetCapabilities(m_codec) : EncoderCapabilities{};
}

Result<FrameBufferPtr> RoiEncoder::create_frame(MemoryType memory)
{
    if (!m_backend || !m_session) {
        return Status::Error("no encoder is open");
    }
    return m_backend->CreateFrame(*m_session.get(), memory);
}

void RoiEncoder::open_deferred(const EncoderConfig& config)
{
    if (config.policy != QualityPolicy::OBJECT_FIRST) {
        // Refused rather than approximated. BALANCED and BITRATE_FIRST both need the
        // bitrate budget of §15, and running OBJECT_FIRST while reporting otherwise would
        // make every measurement taken against it wrong.
        fail(std::format("quality policy {} is not implemented",
            quality_policy_name(config.policy)));
    }
    // Same rule for settings that exist only as declarations. A caller who asks for a
    // bitrate target and gets neither has been misled.

    std::string errors;
    for (const IEncoderBackend& backend : backend_chain(config.kind)) {
        auto opened = backend.Configure(config);
        if (!opened) {
            errors += std::format(" {};", opened.status().message());
            continue;
        }
        // Some backends open and only fail on the first frame. The probe consumes the
        // context either way -- an encoder that has seen a frame cannot be handed back as
        // fresh -- so a successful probe is followed by opening a second, untouched one.
        if (backend.NeedsProbe()) {
            const bool ok = probe_one_frame(backend, *opened.value().get());
            opened.value().reset();
            if (!ok) {
                errors += std::format(" {}: opened but encodes nothing;",
                    backend_name(backend.kind()));
                continue;
            }
            opened = backend.Configure(config);
            if (!opened) {
                errors += std::format(" {};", opened.status().message());
                continue;
            }
        }
        m_backend = &backend;
        m_kind = backend.kind();
        m_session = std::move(opened.value());
        break;
    }
    if (!m_backend) {
        fail(std::format("no {} encoder available:{}",
            config.codec == Codec::H264 ? "H.264" : "HEVC", errors));
    }

    m_codec = config.codec;

    // libx265 and the hardware wrappers hand back packets with no duration. The
    // mp4 muxer derives each sample's duration from the next packet, so with
    // nothing following it the final sample is silently dropped -- one frame
    // short, and the file a frame shorter than the source. MPEG-TS does not
    // care, which is why this only ever showed up in mp4.
    const AVRational fps{config.input.frame_rate.num, std::max(config.input.frame_rate.den, 1)};
    m_frame_duration = av_rescale_q(1, av_inv_q(fps), session().enc.p->time_base);
    if (m_frame_duration <= 0) {
        m_frame_duration = 1;
    }
}

void RoiEncoder::drain()
{
    for (;;) {
        const auto pkt = m_backend->ReceivePacket(*m_session.get());
        if (!pkt) {
            const StatusCode code = pkt.status().code();
            if (code == StatusCode::Again || code == StatusCode::EndOfStream) {
                return;
            }
            fail(pkt.status().message());
        }
        // The public EncodedPacket only borrows; the AVPacket behind it is the session's,
        // and the sink wants that -- it rescales and muxes with libav's own calls.
        AVPacket* raw = session().pkt.p;
        if (raw->duration <= 0) {
            raw->duration = m_frame_duration;
        }
        m_sink->on_packet(raw, session().enc.p->time_base);
    }
}

void RoiEncoder::encode(
    AVFrame* frame, const std::vector<Rect>& rects, int64_t pts,
    std::span<const uint8_t> sei_unregistered)
{
    // The decoder's frame goes in through the same door an embedder uses. Borrowed, not
    // owned: the decoder reuses it for the next frame.
    AvFrameBuffer buffer(frame);
    encode_buffer(buffer, rects, pts, sei_unregistered);
}

void RoiEncoder::encode_buffer(
    IFrameBuffer& buffer, const std::vector<Rect>& rects, int64_t pts,
    std::span<const uint8_t> sei_unregistered)
{
    BackendFrameControl control;
    control.regions = rects;
    control.pts = pts;
    control.sei_unregistered = sei_unregistered;

    const Status s = m_backend->SubmitFrame(*m_session.get(), buffer, control);
    if (!s) {
        fail(s.message());
    }
    drain();
}

void RoiEncoder::flush()
{
    const Status s = m_backend->Flush(*m_session.get());
    if (!s) {
        fail(s.message());
    }
    drain();
}

} // namespace sve
