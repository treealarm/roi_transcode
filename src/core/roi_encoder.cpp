#include "roi_encoder.h"

#include "fail.h"
#include "packet_sink.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace sve {


using roiav::fail;

const char* RoiEncoder::label() const
{
    const bool h264 = m_codec == Codec::H264;
    switch (m_kind) {
    case Backend::IntelOneVpl:
        return h264 ? "h264_qsv (Intel VPL mfxExtEncoderROI)"
                    : "hevc_qsv (Intel VPL mfxExtEncoderROI)";
    case Backend::Vaapi:
        return h264 ? "h264_vaapi (Intel iHD VAEncROI)"
                    : "hevc_vaapi (Intel iHD VAEncROI)";
    case Backend::NvidiaNvenc:
        return h264 ? "h264_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)"
                    : "hevc_nvenc (NVIDIA; stock FFmpeg ignores ROI side data)";
    case Backend::X264:
        return "libx264 (CPU, via libavcodec)";
    case Backend::X264Direct:
        return "libx264 (CPU, direct API, per-macroblock quant_offsets)";
    case Backend::X265Direct:
        return "libx265 (CPU, direct API, per-block quantOffsets)";
    case Backend::X265:
        return "libx265 (CPU, per-block quantOffsets)";
    case Backend::IntelOneVplDirect:
        return h264 ? "h264 (Intel oneVPL, direct API, mfxExtEncoderROI)"
                    : "hevc (Intel oneVPL, direct API, mfxExtEncoderROI)";
    default:
        return h264 ? "libx264 (CPU)" : "libx265 (CPU)";
    }
}

namespace {

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

void RoiEncoder::open(const BackendConfig& config, PacketSink& sink)
{
    open_deferred(config);
    attach(sink);
}

void RoiEncoder::attach(PacketSink& sink)
{
    m_sink = &sink;
    m_sink->begin(m_params);
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

void RoiEncoder::open_deferred(const BackendConfig& config)
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
    const std::vector<BackendRef> chain = resolve_backends(
        config.kind, config.backend_selection, config.codec, &m_backends_skipped);
    if (chain.empty()) {
        std::string why;
        for (const std::string& reason : m_backends_skipped) {
            why += " " + reason + ";";
        }
        fail(why.empty()
            ? std::string("backend selection excludes every backend this build has")
            : "backend selection leaves nothing that can encode this codec:" + why);
    }
    for (const IEncoderBackend& backend : chain) {
        auto opened = backend.Configure(config);
        if (!opened) {
            ++m_backends_declined;
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
                ++m_backends_declined;
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

    // Asked of the backend rather than read off its session: this is what replaced
    // reaching for an AVCodecContext, and it is why a backend on its own SDK can now
    // reach the muxer at all.
    auto params = m_backend->GetCodecParameters(*m_session.get());
    if (!params) {
        fail(params.status().message());
    }
    m_params = std::move(params.value());

    // libx265 and the hardware wrappers hand back packets with no duration. The
    // mp4 muxer derives each sample's duration from the next packet, so with
    // nothing following it the final sample is silently dropped -- one frame
    // short, and the file a frame shorter than the source. MPEG-TS does not
    // care, which is why this only ever showed up in mp4.
    //
    // One frame lasts frame_rate.den/frame_rate.num seconds, expressed in the output time
    // base -- which is av_rescale_q(1, 1/fps, time_base) written out. It is written out
    // because this is the last thing in the core that needed libavutil, and a division
    // with a rounding rule is a poor reason to keep the dependency. AV_ROUND_NEAR_INF is
    // what av_rescale_q applies, and it is (b + c/2) / c for the positive values these
    // always are; the 64-bit intermediate is what keeps a 90 kHz time base exact.
    const int64_t seconds_per_frame_num =
        static_cast<int64_t>(std::max(config.input.frame_rate.den, 1))
        * std::max(m_params.time_base.den, 1);
    const int64_t seconds_per_frame_den =
        static_cast<int64_t>(m_params.time_base.num) * config.input.frame_rate.num;
    m_frame_duration = seconds_per_frame_den > 0
        ? (seconds_per_frame_num + seconds_per_frame_den / 2) / seconds_per_frame_den
        : 0;
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
        // The packet the backend handed back, and nothing reached past it to get here.
        // A duration the encoder left unset is filled in from the frame rate: the mp4
        // muxer derives a sample's duration from the next packet, so with nothing
        // following it the last sample is silently dropped.
        EncodedPacket out = pkt.value();
        if (out.duration <= 0) {
            out.duration = m_frame_duration;
        }
        if (out.time_base.den <= 0) {
            out.time_base = m_params.time_base;
        }
        m_sink->on_packet(out);
    }
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
