#include "sve/encoder.h"

#include "av_raii.h"
#include "bitrate_budget.h"
#include "box_smoother.h"
#include "codec_id.h"
#include "encoder_runtime_warnings.h"
#include "frame_roi_pipeline.h"
#include "importance_map.h"
#include "object_normalizer.h"
#include "packet_sink.h"
#include "qp_slew.h"
#include "roi_encoder.h"
#include "roi_lag.h"
#include "sve/metadata.h"

#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

namespace sve {

namespace {

/// Depth of the packet queue between the backend and the caller (§24 asks for a bounded
/// one). Generous rather than tight: a backend with lookahead can hand back a burst after
/// a quiet stretch, and the caller drains between frames anyway.
constexpr std::size_t kMaxQueuedPackets = 64;

} // namespace

const char* encoder_state_name(EncoderState s)
{
    switch (s) {
    case EncoderState::Created:     return "created";
    case EncoderState::Configured:  return "configured";
    case EncoderState::Running:     return "running";
    case EncoderState::Draining:    return "draining";
    case EncoderState::EndOfStream: return "end-of-stream";
    case EncoderState::Stopped:     return "stopped";
    case EncoderState::Failed:      return "failed";
    }
    return "?";
}

/// The four stateful collaborators plus the pipeline over them, in one place.
///
/// They used to be assembled separately by the file path and by the session path, each
/// constructing the pipeline inside its own per-frame loop. That was the same wiring twice,
/// and the second copy is where the two drifted: only one of them fed the bitrate budget
/// from its own output. Here there is one.
struct SmartVideoEncoder::Impl : PacketSink {
    explicit Impl(const SmartEncoderConfig& cfg) : config(cfg) {}

    SmartEncoderConfig config;
    EncoderConfig backend_config;

    RoiEncoder encoder;
    BitrateBudget bitrate;
    QpSlewLimiter qp_slew;
    BoxSmoother box_smooth;
    RoiMapBuilder roi_builder;

    std::deque<EncodedPacket> queued;
    std::vector<RoiLagSample> lag_samples;
    EncoderStatistics stats;
    EncoderState state = EncoderState::Created;
    Status fatal;

    Codec out_codec = Codec::H265;
    double frame_sec = 1.0 / 25.0;
    bool key_frame_pending = false;

    bool warned_roi = false;
    bool warned_roi_dropped = false;
    bool warned_sei = false;

    // --- PacketSink: where the backend's output lands until the caller asks for it. ---

    void begin(AVCodecContext* enc) override
    {
        out_codec = codec_from_av_id(enc->codec_id);
    }

    void on_packet(AVPacket* pkt, AVRational tb) override
    {
        if (bitrate.active()) {
            double dt = 0.0;
            if (pkt->duration > 0 && tb.den > 0) {
                dt = static_cast<double>(pkt->duration) * tb.num / tb.den;
            }
            bitrate.observe(pkt->size, dt > 0.0 ? dt : frame_sec);
        }

        EncodedPacket out;
        out.payload = PacketBuffer::Copy(std::span<const uint8_t>(
            pkt->data, static_cast<std::size_t>(pkt->size)));
        out.pts = pkt->pts;
        out.dts = pkt->dts;
        out.duration = pkt->duration;
        out.key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        out.codec = out_codec;
        out.time_base = Rational{tb.num, tb.den};

        ++stats.packets_received;
        stats.bytes_encoded += pkt->size;
        queued.push_back(std::move(out));
    }

    void end() override {}

    bool queue_full() const { return queued.size() >= kMaxQueuedPackets; }

    /// ROI for one frame, plus the guards that must be audible when it degrades.
    std::vector<Rect> rects_for(const FrameMetadata& meta)
    {
        const EncoderCapabilities caps = encoder.capabilities();

        if (!meta.regions.qp_map.deltas.empty()) {
            int32_t dropped = 0;
            const int32_t budget = effective_region_budget(config.settings.quality, caps);
            auto rects = rects_from_qp_map(meta.regions.qp_map, budget, &dropped);
            stats.regions_dropped += dropped;
            warn_roi_regions_dropped_once(
                stats.regions_dropped, caps, out_codec, warned_roi_dropped);
            return rects;
        }

        FrameRoiPipelineInput in{
            .boxes = meta.regions.boxes,
            .quality = config.settings.quality,
            .temporal = config.settings.temporal,
            .caps = caps,
            .width = config.input.width,
            .height = config.input.height,
            .frame_interval_sec = frame_sec,
        };
        FrameRoiPipeline pipeline(bitrate, box_smooth, qp_slew, roi_builder);
        FrameRoiPipelineOutput out = pipeline.process(in);
        stats.regions_dropped += out.regions_dropped;
        warn_roi_regions_dropped_once(
            stats.regions_dropped, caps, out_codec, warned_roi_dropped);
        lag_samples.insert(
            lag_samples.end(), out.lag_samples.begin(), out.lag_samples.end());
        last_applied = std::move(out.applied_boxes);
        last_block = out.coding_block;
        log_roi_once(out.rects, caps);
        return std::move(out.rects);
    }

    std::vector<Box> last_applied;
    int32_t last_block = 16;

    /// One line describing the region map, on the first frame that has one.
    ///
    /// It reads as a diagnostic and is also a contract: integration/scripts/check.sh
    /// asserts the region count from it, which is how the per-codec Intel limit is checked
    /// without a GPU-specific crash to look for.
    void log_roi_once(const std::vector<Rect>& rects, const EncoderCapabilities& caps)
    {
        if (warned_roi_map) {
            return;
        }
        QpDeltaMapBuilder builder;
        const auto grid =
            builder.build(config.input.width, config.input.height, last_block, rects);
        std::fprintf(stderr, "ROI: %zu rects, qpDeltaMap %dx%d blocks of %d%s\n",
            rects.size(), grid.cols, grid.rows, last_block,
            caps.qp_map ? " (qp_map)" : "");
        warned_roi_map = true;
    }
    bool warned_roi_map = false;

    std::vector<uint8_t> sei_for(int64_t pts)
    {
        const EncoderCapabilities caps = encoder.capabilities();
        warn_sei_unsupported_once(config.settings.metadata.bitstream_sei, caps, warned_sei);
        if (!config.settings.metadata.bitstream_sei || !caps.sei) {
            return {};
        }
        SeiFrame meta;
        meta.frame_id = static_cast<uint64_t>(stats.frames_submitted);
        meta.pts = pts;
        meta.objects =
            sei_objects_from_boxes(last_applied, config.settings.metadata.max_objects);
        return encode_sei_side_data(meta, config.settings.metadata.max_objects);
    }
};

SmartVideoEncoder::SmartVideoEncoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
SmartVideoEncoder::~SmartVideoEncoder() = default;

Result<std::unique_ptr<SmartVideoEncoder>> SmartVideoEncoder::Create(
    const SmartEncoderConfig& config)
{
    if (!config.input.valid()) {
        return Status::InvalidArgument("input video format is not usable");
    }

    auto impl = std::make_unique<Impl>(config);

    EncoderConfig& ep = impl->backend_config;
    ep.input = config.input;
    ep.kind = config.settings.encoder;
    ep.codec = config.settings.codec;
    ep.policy = config.settings.policy;
    ep.temporal = config.settings.temporal;
    ep.rate_control = config.settings.rate_control;
    ep.crf = config.settings.crf;
    ep.gop = config.settings.gop;
    ep.preset = config.settings.preset;

    if (ep.codec == Codec::SameAsSource) {
        // No source to follow here: this entry point is handed frames, not a file. A caller
        // transcoding a clip resolves it against the input's own codec before Create.
        return Status::InvalidArgument(
            "Codec::SameAsSource needs a source; resolve it before creating an encoder");
    }

    // See STATUS.md, decision Р5: the CPU HEVC target-bitrate path is unstable, so it is
    // refused rather than half-honoured, and the refusal is both logged and counted.
    if (ep.kind == Encoder::Cpu && ep.codec == Codec::H265
        && ep.rate_control.target_bitrate > 0) {
        impl->stats.cpu_h265_target_guard = true;
        ep.rate_control.target_bitrate = 0;
        std::fprintf(stderr, "guard: cpu h265 target-bitrate disabled; using CRF/CQP\n");
    }

    impl->bitrate = BitrateBudget(ep.rate_control);
    if (config.input.frame_rate.num > 0 && config.input.frame_rate.den > 0) {
        impl->frame_sec = static_cast<double>(config.input.frame_rate.den)
            / static_cast<double>(config.input.frame_rate.num);
    }

    try {
        impl->encoder.open(ep, *impl);
    } catch (const std::exception& e) {
        return Status::DeviceUnavailable(e.what());
    }

    impl->stats.encoder_used = impl->encoder.kind();
    impl->stats.encoder_name = impl->encoder.label();
    impl->state = EncoderState::Configured;
    return std::unique_ptr<SmartVideoEncoder>(new SmartVideoEncoder(std::move(impl)));
}

Status SmartVideoEncoder::SubmitFrame(const AVFrame* frame, const FrameMetadata& metadata)
{
    Impl& s = *m_impl;
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state != EncoderState::Configured && s.state != EncoderState::Running) {
        return Status::InvalidArgument(
            std::string("SubmitFrame in state ") + encoder_state_name(s.state));
    }
    if (frame == nullptr) {
        return Status::InvalidArgument("SubmitFrame needs a frame");
    }
    if (s.queue_full()) {
        // Backpressure by §24: a code, not a block. The caller drains and comes back.
        return Status::Again();
    }

    s.state = EncoderState::Running;
    const int64_t pts = metadata.timestamp.valid() ? metadata.timestamp.value : 0;

    try {
        const std::vector<Rect> rects = s.rects_for(metadata);
        if (!s.warned_roi && s.encoder.kind() == Encoder::Nvenc) {
            std::fprintf(stderr,
                "warning: stock FFmpeg hevc_nvenc ignores ROI; "
                "qpDeltaMap is built but not submitted to NVENC SDK\n");
            s.warned_roi = true;
        }
        const std::vector<uint8_t> sei = s.sei_for(pts);
        if (metadata.force_key_frame || s.key_frame_pending) {
            ++s.stats.keyframes_requested;
            s.key_frame_pending = false;
        }
        // const_cast: the encoder only reads these pixels. The signature is const because
        // §8.1 promises the caller's frame is not modified, and libavcodec's send takes a
        // non-const pointer it likewise only reads.
        s.encoder.encode(const_cast<AVFrame*>(frame), rects, pts, sei);
    } catch (const std::exception& e) {
        s.fatal = Status::BackendError(e.what());
        s.state = EncoderState::Failed;
        return s.fatal;
    }

    ++s.stats.frames_submitted;
    s.stats.frames_converted = s.encoder.frames_converted();
    s.stats.frames_uploaded = s.encoder.frames_uploaded();
    return Status::Ok();
}

Status SmartVideoEncoder::ReceivePacket(EncodedPacket* packet)
{
    Impl& s = *m_impl;
    if (packet == nullptr) {
        return Status::InvalidArgument("ReceivePacket needs somewhere to put the packet");
    }
    if (!s.queued.empty()) {
        *packet = std::move(s.queued.front());
        s.queued.pop_front();
        return Status::Ok();
    }
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state == EncoderState::Draining || s.state == EncoderState::EndOfStream) {
        s.state = EncoderState::EndOfStream;
        return Status::EndOfStream();
    }
    return Status::Again();
}

Status SmartVideoEncoder::Flush()
{
    Impl& s = *m_impl;
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state == EncoderState::Draining || s.state == EncoderState::EndOfStream
        || s.state == EncoderState::Stopped) {
        return Status::Ok();
    }
    try {
        s.encoder.flush();
    } catch (const std::exception& e) {
        s.fatal = Status::BackendError(e.what());
        s.state = EncoderState::Failed;
        return s.fatal;
    }
    s.state = EncoderState::Draining;
    return Status::Ok();
}

Status SmartVideoEncoder::Stop()
{
    Impl& s = *m_impl;
    if (s.state == EncoderState::Stopped) {
        return Status::Ok();
    }
    s.queued.clear();
    s.state = EncoderState::Stopped;
    return Status::Ok();
}

Status SmartVideoEncoder::RequestKeyFrame()
{
    Impl& s = *m_impl;
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state == EncoderState::Stopped || s.state == EncoderState::EndOfStream) {
        return Status::InvalidArgument(
            std::string("RequestKeyFrame in state ") + encoder_state_name(s.state));
    }
    s.key_frame_pending = true;
    return Status::Ok();
}

Status SmartVideoEncoder::Reconfigure(const RuntimeEncoderSettings& settings)
{
    Impl& s = *m_impl;
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state != EncoderState::Configured && s.state != EncoderState::Running) {
        return Status::InvalidArgument(
            std::string("Reconfigure in state ") + encoder_state_name(s.state));
    }

    if (settings.frame_rate) {
        const Rational fps = *settings.frame_rate;
        if (fps.num <= 0 || fps.den <= 0) {
            return Status::InvalidArgument("frame rate must be positive");
        }
        s.frame_sec = static_cast<double>(fps.den) / static_cast<double>(fps.num);
    }
    if (settings.quality) {
        s.config.settings.quality = *settings.quality;
    }
    if (settings.target_bitrate) {
        RateControlPolicy rc = s.config.settings.rate_control;
        rc.target_bitrate = *settings.target_bitrate;
        if (s.encoder.kind() == Encoder::Cpu && s.out_codec == Codec::H265
            && rc.target_bitrate > 0) {
            // Same guard as at Create, for the same reason -- and it has to answer here
            // rather than quietly ignore, because the caller is asking now.
            return Status::Unsupported(
                "cpu h265 has no target-bitrate mode in this build; see STATUS.md Р5");
        }
        s.config.settings.rate_control = rc;
        // The budget is an outer loop over the encoder's constant quantizer, so a new
        // target takes effect from the next frame without reopening anything.
        s.bitrate = BitrateBudget(rc);
    }
    return Status::Ok();
}

Status SmartVideoEncoder::GetCodecParameters(CodecParameters* parameters) const
{
    if (parameters == nullptr) {
        return Status::InvalidArgument("GetCodecParameters needs somewhere to write");
    }
    const AVCodecContext* ctx = m_impl->encoder.ctx();
    if (ctx == nullptr) {
        return Status::InvalidArgument("no encoder is open");
    }
    parameters->codec = codec_from_av_id(ctx->codec_id);
    parameters->width = ctx->width;
    parameters->height = ctx->height;
    parameters->time_base = Rational{ctx->time_base.num, ctx->time_base.den};
    if (ctx->extradata != nullptr && ctx->extradata_size > 0) {
        parameters->extradata = PacketBuffer::Copy(std::span<const uint8_t>(
            ctx->extradata, static_cast<std::size_t>(ctx->extradata_size)));
    }
    return Status::Ok();
}

EncoderCapabilities SmartVideoEncoder::GetCapabilities() const
{
    return m_impl->encoder.capabilities();
}

EncoderState SmartVideoEncoder::State() const { return m_impl->state; }

EncoderStatistics SmartVideoEncoder::GetStatistics() const
{
    EncoderStatistics out = m_impl->stats;
    Report lag;
    fill_lag_report(m_impl->lag_samples, lag);
    out.roi_lag_measured = lag.roi_lag_measured;
    out.roi_lag_samples = lag.roi_lag_samples;
    out.roi_lag_min_iou = lag.roi_lag_min_iou;
    out.roi_lag_p95_frames = lag.roi_lag_p95_frames;
    out.roi_lag_p99_frames = lag.roi_lag_p99_frames;
    out.roi_lag_p95_ms = lag.roi_lag_p95_ms;
    out.roi_lag_p99_ms = lag.roi_lag_p99_ms;
    out.roi_lag_acceptance = lag.roi_lag_acceptance;
    return out;
}

Report SmartVideoEncoder::ToReport() const
{
    const EncoderStatistics s = GetStatistics();
    Report r;
    r.encoder_used = s.encoder_used;
    r.encoder_name = s.encoder_name;
    r.frames = s.frames_submitted;
    r.packets = s.packets_received;
    r.bytes = s.bytes_encoded;
    r.regions_dropped = s.regions_dropped;
    r.frames_converted = s.frames_converted;
    r.frames_uploaded = s.frames_uploaded;
    r.cpu_h265_target_guard = s.cpu_h265_target_guard;
    r.roi_lag_measured = s.roi_lag_measured;
    r.roi_lag_samples = s.roi_lag_samples;
    r.roi_lag_min_iou = s.roi_lag_min_iou;
    r.roi_lag_p95_frames = s.roi_lag_p95_frames;
    r.roi_lag_p99_frames = s.roi_lag_p99_frames;
    r.roi_lag_p95_ms = s.roi_lag_p95_ms;
    r.roi_lag_p99_ms = s.roi_lag_p99_ms;
    r.roi_lag_acceptance = s.roi_lag_acceptance;
    return r;
}

} // namespace sve
