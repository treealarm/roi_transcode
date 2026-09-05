#include "sve/encoder.h"

#include "bitrate_budget.h"
#include "box_smoother.h"
#include "encoder_runtime_warnings.h"
#include "frame_roi_pipeline.h"
#include "importance_map.h"
#include "logger.h"
#include "object_normalizer.h"
#include "packet_sink.h"
#include "qp_slew.h"
#include "encoder_assembly.h"
#include "roi_encoder.h"
#include "roi_lag.h"
#include "sve/metadata.h"

#include <format>
#include <deque>
#include <map>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace sve {

namespace {

/// The policy the outer loop should run under, given what the backend does for itself.
///
/// §15's loop exists for encoders that have no bitrate mode of their own. Where one does,
/// the target belongs to it: this returns the same policy with the target cleared, which is
/// how `BitrateBudget` is told to stand down (`active()` is a non-zero target).
RateControlPolicy outer_loop_policy(const RateControlPolicy& policy, bool native_bitrate)
{
    if (!native_bitrate) {
        return policy;
    }
    RateControlPolicy off = policy;
    off.target_bitrate = 0;
    return off;
}

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
    Impl(const EncoderConfig& cfg, const BackendRegistry& registry)
        : config(cfg)
        , encoder(registry)
    {
    }

    EncoderConfig config;
    BackendConfig backend_config;

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

    Logger log;
    FallbackPolicy policy = FallbackPolicy::Permissive;

    Codec out_codec = Codec::H265;
    double frame_sec = 1.0 / 25.0;
    bool key_frame_pending = false;

    bool warned_roi = false;
    bool warned_roi_dropped = false;
    bool warned_sei = false;
    bool warned_qp_map = false;
    bool warned_qp_clamped = false;

    /// Counts a degradation the first time it is applied, so fallbacks_applied is a number
    /// of distinct capabilities worked around rather than of frames.
    void count_fallback(bool& already)
    {
        if (!already) {
            ++stats.fallbacks_applied;
        }
    }

    // --- PacketSink: where the backend's output lands until the caller asks for it. ---

    void begin(const CodecParameters& params) override
    {
        out_codec = params.codec;
    }

    void on_packet(const EncodedPacket& packet) override
    {
        if (bitrate.active()) {
            double dt = 0.0;
            const Rational tb = packet.time_base;
            if (packet.duration > 0 && tb.den > 0) {
                dt = static_cast<double>(packet.duration) * tb.num / tb.den;
            }
            bitrate.observe(static_cast<int64_t>(packet.payload.size()),
                dt > 0.0 ? dt : frame_sec);
            // §50: said once, and counted, the moment the loop runs out of authority. A
            // target that cannot be reached because the expensive part of the frame is
            // protected is a degradation the caller is entitled to hear about -- until now
            // the only evidence was the file being the wrong size.
            if (bitrate.target_unreachable() && !stats.bitrate_target_unreachable) {
                stats.bitrate_target_unreachable = true;
                ++stats.fallbacks_applied;
                log.warn("bitrate target unreachable: the outer loop is at its limit and "
                    "the stream is still above the target band -- the protected regions "
                    "carry more of the frame's cost than the background can pay back");
            }
        }

        EncodedPacket out = packet;
        // The backend reports the codec it opened; out_codec is what the caller asked for
        // and what the rest of this class reasons about, and they agree by construction.
        out.codec = out_codec;

        ++stats.packets_received;
        stats.bytes_encoded += static_cast<int64_t>(packet.payload.size());
        write_sidecar(packet.pts);
        queued.push_back(std::move(out));
    }

    void end() override
    {
        // Two different failures, counted apart because they are not the same fault and a
        // single number would be both wrong and ambiguous.
        //
        // `sidecar_unmatched_packets` is a packet arriving under a timestamp nothing was
        // submitted with. `pending_sidecar` is the reverse: a frame submitted whose packet
        // never came. A backend that skews timestamps produces one of each per frame, and
        // adding them reported eight for four frames until 2026-09-04 -- the same frame
        // counted once on the way in and once on the way out.
        const auto never_emitted = static_cast<int64_t>(pending_sidecar.size());
        pending_sidecar.clear();
        // Only once the stream was actually finished. §8.13 makes Stop safe from any
        // state, so it is also the cleanup path of a cancelled job -- and frames still
        // inside a lookahead when the caller gave up are not a broken sidecar, they are an
        // encode nobody drained. Warning there would report a data-integrity fault about a
        // decision the caller made on purpose.
        const bool stream_was_finished =
            state == EncoderState::Draining || state == EncoderState::EndOfStream;
        if (never_emitted > 0 && stream_was_finished) {
            log.warn(std::format("sidecar: {} frame(s) submitted whose packet never "
                                 "arrived; records and packets do not correspond",
                never_emitted));
        }
        // Unconditional, unlike the above: a packet that arrived under a timestamp nobody
        // submitted is wrong however the encode ended.
        if (sidecar_unmatched_packets > 0) {
            log.warn(std::format("sidecar: {} packet(s) arrived under a timestamp no frame "
                                 "was submitted with; records and packets do not correspond",
                sidecar_unmatched_packets));
        }
    }

    /// The record for the packet just emitted, if a sidecar was asked for.
    void write_sidecar(int64_t pts)
    {
        if (config.settings.metadata.sidecar == SidecarFormat::None
            || !config.settings.metadata.sidecar_sink) {
            return;
        }
        const auto it = pending_sidecar.find(pts);
        if (it == pending_sidecar.end()) {
            // A packet whose frame we never held. Not silently skipped: §19.3's guarantee
            // is that records correspond to packets, and a missing one breaks it. Counted
            // separately from the frames that never produced a packet -- see end().
            ++sidecar_unmatched_packets;
            return;
        }
        config.settings.metadata.sidecar_sink(encode_sidecar_ndjson(
            it->second, sidecar_index, config.settings.metadata.max_objects));
        ++sidecar_index;
        pending_sidecar.erase(it);
    }

    bool queue_full() const { return queued.size() >= kMaxQueuedPackets; }

    /// How the pixels reach the backend. The two public overloads differ in nothing but
    /// this call, so everything around it -- state checks, backpressure, regions, SEI,
    /// keyframe requests, failure handling -- is written once.
    using Handover = std::function<void(
        const std::vector<Rect>& rects, int64_t pts, std::span<const uint8_t> sei)>;

    Status submit(const FrameMetadata& metadata, const Handover& hand);

    /// ROI for one frame, plus the guards that must be audible when it degrades.
    Status rects_for(const FrameMetadata& meta, std::vector<Rect>* out_rects)
    {
        const EncoderCapabilities caps = encoder.capabilities();
        const bool have_map = !meta.regions.qp_map.deltas.empty();

        if (const Status s =
                guard_qp_map_quantized(have_map, caps, policy, log, warned_qp_map);
            !s.ok()) {
            return s;
        }
        if (have_map && !caps.qp_map) {
            count_fallback(warned_qp_map);
        }

        if (have_map) {
            int32_t dropped = 0;
            const int32_t budget = effective_region_budget(config.settings.quality, caps);
            *out_rects = rects_from_qp_map(meta.regions.qp_map, budget, &dropped);
            stats.regions_dropped += dropped;
            if (dropped > 0) {
                count_fallback(warned_roi_dropped);
            }
            log_roi_once(*out_rects, caps);
            return guard_roi_regions_dropped(
                stats.regions_dropped, caps, out_codec, policy, log, warned_roi_dropped);
        }

        // A mask is the second form Regions accepts, and it has to be sampled onto the
        // block grid rather than fed to the box pipeline: it carries no kinds to weigh, so
        // it interpolates between the background and the strongest value the settings name.
        //
        // Leaving this out is what the migration of Session found: a caller passing a mask
        // got no regions at all, quietly, because the box path had nothing to work from.
        if (meta.regions.mask.consistent()) {
            const Quality quality = bitrate.apply(config.settings.quality);
            MaskMapping mapping;
            mapping.qp_delta_at_0 = quality.background_qp_delta;
            mapping.qp_delta_at_255 = quality.mask_qp_delta_at_255;
            mapping.levels = quality.mask_levels;
            const int32_t block = effective_coding_block(caps);
            const QpDeltaMap grid = qp_map_from_mask(meta.regions.mask, mapping,
                config.input.width, config.input.height, block);
            int32_t dropped = 0;
            const int32_t budget = effective_region_budget(config.settings.quality, caps);
            *out_rects = rects_from_qp_map(grid, budget, &dropped);
            stats.regions_dropped += dropped;
            if (dropped > 0) {
                count_fallback(warned_roi_dropped);
            }
            last_block = block;
            log_roi_once(*out_rects, caps);
            return guard_roi_regions_dropped(
                stats.regions_dropped, caps, out_codec, policy, log, warned_roi_dropped);
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
        if (out.regions_dropped > 0) {
            count_fallback(warned_roi_dropped);
        }
        if (const Status s = guard_roi_regions_dropped(stats.regions_dropped, caps,
                out_codec, policy, log, warned_roi_dropped);
            !s.ok()) {
            return s;
        }
        lag_samples.insert(
            lag_samples.end(), out.lag_samples.begin(), out.lag_samples.end());
        last_block = out.coding_block;
        log_roi_once(out.rects, caps);
        *out_rects = std::move(out.rects);
        return Status::Ok();
    }

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
        log.info(std::format("ROI: {} rects, qpDeltaMap {}x{} blocks of {}{}",
            rects.size(), grid.cols, grid.rows, last_block,
            caps.qp_map ? " (qp_map)" : ""));
        warned_roi_map = true;
    }
    bool warned_roi_map = false;

    /// Submitted metadata waiting for the packet it belongs to -- §19.3.
    ///
    /// Keyed by PTS because that is §19.4's first synchronisation key and the only one the
    /// packet carries back. Submission order is not emission order once a codec has
    /// lookahead or B-frames, so writing the record at submit would produce a file whose
    /// Nth line is not the Nth packet -- which is the one thing §19.3 asks of it.
    std::map<int64_t, SeiFrame> pending_sidecar;
    /// Position of the packet this record accompanies, which is what §19.3 calls
    /// frame_index. Counts emissions, not submissions.
    uint64_t sidecar_index = 0;
    /// Packets that arrived under a timestamp no frame was submitted with.
    ///
    /// The mirror of `pending_sidecar` left non-empty at end(), and counted apart from it
    /// on purpose: one is a record with no packet, the other a packet with no record. They
    /// were one number until 2026-09-04, which reported eight faults for four frames.
    int64_t sidecar_unmatched_packets = 0;

    /// §19.1 keeps `objects` and `roi_info` as separate fields, and §19.2's payload is the
    /// objects -- so what goes in is what the caller submitted, not what the pipeline made
    /// of it after smoothing and normalisation. The two entry points disagreed about this
    /// until they were built on one encoder: the file path embedded the applied boxes, the
    /// session path the submitted ones. The session path was right.
    /// The metadata for one frame, for both channels §19 defines.
    ///
    /// Built once and used twice on purpose. The sidecar exists precisely for the case
    /// where the bitstream cannot carry the objects -- §19.3 opens with "если SEI
    /// недоступен или отключён" -- so it must not be reachable only through the SEI path.
    /// Deriving both from one SeiFrame is also what keeps the two channels describing the
    /// same frame the same way; the file path and the session path once disagreed about
    /// exactly this, and §19.1's separation of `objects` from `roi_info` is the rule they
    /// broke. What goes in is what the caller submitted.
    Status metadata_for(const FrameMetadata& meta, int64_t pts, std::vector<uint8_t>* out)
    {
        const MetadataConfig& cfg = config.settings.metadata;
        const bool want_sidecar = cfg.sidecar != SidecarFormat::None && cfg.sidecar_sink;

        const EncoderCapabilities caps = encoder.capabilities();
        const bool wanted = cfg.bitstream_sei;
        if (wanted && !caps.sei) {
            count_fallback(warned_sei);
        }
        if (const Status s = guard_sei_unsupported(wanted, caps, policy, log, warned_sei);
            !s.ok()) {
            return s;
        }

        const bool want_sei = wanted && caps.sei;
        if (!want_sei && !want_sidecar) {
            return Status::Ok();
        }

        SeiFrame frame;
        frame.frame_id = static_cast<uint64_t>(stats.frames_submitted);
        frame.pts = pts;
        frame.objects = sei_objects_from_boxes(meta.regions.boxes, cfg.max_objects);
        if (want_sei) {
            *out = encode_sei_side_data(frame, cfg.max_objects);
        }
        if (want_sidecar) {
            // Held until this frame's packet comes out; see write_sidecar. A second frame
            // at the same PTS replaces the first, which is the only sane reading of two
            // frames claiming one timestamp.
            pending_sidecar[pts] = std::move(frame);
        }
        return Status::Ok();
    }
};

SmartVideoEncoder::SmartVideoEncoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
SmartVideoEncoder::~SmartVideoEncoder() = default;

std::unique_ptr<SmartVideoEncoder> CreateVideoEncoder(const EncoderConfig& config)
{
    return CreateVideoEncoderIn(config, default_backend_registry());
}

std::unique_ptr<SmartVideoEncoder> CreateVideoEncoderIn(
    const EncoderConfig& config, const BackendRegistry& registry)
{
    if (!config.input.valid()) {
        throw EncoderError(Status::InvalidArgument("input video format is not usable"));
    }

    // §19.3 asks for a sink when it asks for a sidecar. Refused here rather than accepted
    // and dropped: a caller who set the format and forgot the sink would otherwise get a
    // successful encode and no metadata, which is the shape of failure §50 forbids.
    if (config.settings.metadata.sidecar != SidecarFormat::None
        && !config.settings.metadata.sidecar_sink) {
        throw EncoderError(Status::InvalidArgument(
            "a sidecar format was requested without a sidecar sink to write it to"));
    }

    auto impl = std::make_unique<SmartVideoEncoder::Impl>(config, registry);
    impl->log = Logger(config.log);
    impl->policy = config.settings.fallback;

    BackendConfig& ep = impl->backend_config;
    ep.input = config.input;
    ep.backend_selection = config.settings.backend_selection;
    ep.kind = config.settings.encoder;
    ep.codec = config.settings.codec;
    ep.policy = config.settings.policy;
    ep.temporal = config.settings.temporal;
    ep.rate_control = config.settings.rate_control;
    ep.crf = config.settings.crf;
    ep.gop = config.settings.gop;
    ep.preset = config.settings.preset;

    // §8.2: P010 needs Main10, and a profile that cannot carry the input's depth is
    // refused here rather than producing a file nothing will play.
    {
        const bool ten_bit = config.input.pixel_format == PixelFormat::P010;
        const CodecProfile profile = config.settings.profile;
        if (ten_bit && profile != CodecProfile::Auto && profile != CodecProfile::H265Main10) {
            throw EncoderError(Status::InvalidArgument(std::string("P010 input needs the h265-main10 "
                "profile, not ") + codec_profile_name(profile)));
        }
        if (!ten_bit && profile == CodecProfile::H265Main10) {
            throw EncoderError(Status::InvalidArgument(
                "the h265-main10 profile needs ten-bit input; this one is eight"));
        }
        if (ten_bit && ep.codec == Codec::H264) {
            // §15.3's ten-bit profile is HEVC. H.264 has a ten-bit profile of its own, but
            // nothing in this specification asks for it, and claiming it would be inventing
            // a contract.
            throw EncoderError(Status::InvalidArgument("ten-bit input is supported for H.265 only"));
        }
        ep.profile = profile;
    }

    // §8.4: the preference list is the caller's to give, and an unusable one is refused
    // here rather than turned into a choice nobody asked for.
    if (ep.kind == Backend::Auto) {
        const std::vector<Backend>& want = ep.backend_selection.preferred;
        if (want.empty()) {
            throw EncoderError(Status::InvalidArgument(
                "Backend::Auto needs a backend preference order; the list is empty"));
        }
        for (auto it = want.begin(); it != want.end(); ++it) {
            if (*it == Backend::Auto) {
                throw EncoderError(Status::InvalidArgument(
                    "Backend::Auto cannot appear inside its own preference order"));
            }
            if (std::find(want.begin(), it, *it) != it) {
                throw EncoderError(Status::InvalidArgument(
                    "the backend preference order lists the same backend twice"));
            }
        }
    }

    if (ep.codec == Codec::SameAsSource) {
        // No source to follow here: this entry point is handed frames, not a file. A caller
        // transcoding a clip resolves it against the input's own codec before Create.
        throw EncoderError(Status::InvalidArgument(
            "Codec::SameAsSource needs a source; resolve it before creating an encoder"));
    }

    // §8.4: named outright and unable to produce this codec -- X264 asked for HEVC, say --
    // is Unsupported. Under Auto the same backend is merely skipped, which is why this
    // check is here and not in the chain walk.
    if (ep.kind != Backend::Auto && !backend_supports(ep.kind, ep.codec, registry)) {
        throw EncoderError(Status::Unsupported(std::format("the {} backend cannot encode {}",
            backend_name(ep.kind), ep.codec == Codec::H264 ? "H.264" : "HEVC")));
    }

    if (config.input.frame_rate.num > 0 && config.input.frame_rate.den > 0) {
        impl->frame_sec = static_cast<double>(config.input.frame_rate.den)
            / static_cast<double>(config.input.frame_rate.num);
    }

    try {
        impl->encoder.open(ep, *impl);
    } catch (const EncoderError&) {
        throw;
    } catch (const std::exception& e) {
        // RoiEncoder reports an exhausted chain by throwing, and its message already names
        // every backend that declined and why.
        throw EncoderError(Status::DeviceUnavailable(e.what()));
    }

    // See STATUS.md, decision Р5: the libx265 target-bitrate path is unstable, so it is
    // refused rather than half-honoured, and the refusal is both logged and counted.
    //
    // Asked of the backend that opened, not of the one that was requested. Those were the
    // same question while there was a single Cpu backend named outright, and they stopped
    // being the same the moment selection could arrive at libx265 on its own -- which it
    // always could: Auto on a machine with no Quick Sync walks down to the software
    // encoder, and the guard for the path that is known to be unstable would not have
    // fired. Reading it off the opened backend is what makes it hold for every route in.
    // Keyed on the capability, not on which backend it is. It named Backend::X265 until a
    // second HEVC software backend existed, and X265Direct -- which has no native bitrate
    // mode either -- walked straight past it and accepted a target it does not honour.
    // That is the silent degradation §50 forbids, produced by a guard asking "who are you"
    // where it meant "what can you do".
    if (!impl->encoder.capabilities().native_bitrate && ep.codec == Codec::H265
        && ep.rate_control.target_bitrate > 0) {
        if (const Status s = guard_cpu_h265_target(true, impl->policy, impl->log); !s.ok()) {
            throw EncoderError(s);
        }
        impl->stats.cpu_h265_target_guard = true;
        ++impl->stats.fallbacks_applied;
        ep.rate_control.target_bitrate = 0;
    }

    // After the guard, so a target it disallowed does not reach the budget loop. The
    // backend has already been configured, and ignores the target for HEVC anyway: for
    // libx265 the rate is CRF plus this outer loop, never the encoder's own bitrate mode.
    //
    // And only where there is no rate control underneath. A backend that reports
    // `native_bitrate` is already holding the target with its own ABR and VBV, and this
    // loop's actuator -- a quantiser offset on the background -- then acts on top of an
    // allocator that is itself reacting to the result. Two controllers on one plant, each
    // treating the other's correction as content: measured on a 36 s crowd scene at 3 Mbps,
    // x265direct came out at P95 120% of target with both running and 113% with only its
    // own, and its average went from 79% of target to 99% (docs/STATUS.md, срез Z13).
    impl->bitrate = BitrateBudget(outer_loop_policy(ep.rate_control,
        impl->encoder.capabilities().native_bitrate));

    impl->stats.encoder_used = impl->encoder.kind();
    impl->stats.backend_name = impl->encoder.label();
    impl->stats.backends_declined = impl->encoder.backends_declined();
    for (const std::string& reason : impl->encoder.backends_skipped()) {
        impl->log.warn("backend skipped: " + reason);
    }
    // The kind first, then the prose. The kind is what the caller selected and what a
    // script can read back; the label is a sentence for a person, and two backends can
    // legitimately start it with the same word -- x264 and x264direct both say "libx264",
    // which is true of both and useless for telling them apart.
    impl->log.info(std::format("backend: {} ({}) opened; {} declined before it",
        backend_name(impl->stats.encoder_used), impl->encoder.label(),
        impl->stats.backends_declined));

    // §29's "capability report", which until 2026-09-02 existed only as a number a caller
    // could fetch. It is the answer every fallback below is decided against, so a log that
    // records the fallbacks and not the capabilities they came from is a log that cannot be
    // read back to a cause. At Debug: it is one line per encode, not per frame, but it is
    // detail rather than news, and the built-in sink stays as quiet as it was.
    {
        const EncoderCapabilities& c = impl->encoder.capabilities();
        impl->log.debug(std::format(
            "capabilities: regions={} roi_rect={} qp_map={} ({}x{}) ctu_qp={} "
            "emphasis={} runtime_roi={} sei={} native_bitrate={} main10={} p010={}",
            c.max_roi_regions, c.roi_rectangles, c.qp_map, c.qp_map_block_width,
            c.qp_map_block_height, c.ctu_qp, c.emphasis_map, c.runtime_roi_update, c.sei,
            c.native_bitrate, c.main10, c.p010_input));
    }

    impl->state = EncoderState::Configured;
    return std::unique_ptr<SmartVideoEncoder>(new SmartVideoEncoder(std::move(impl)));
}

Status SmartVideoEncoder::SubmitFrame(TimedFrame frame)
{
    if (!frame.frame) {
        return Status::InvalidArgument("SubmitFrame needs a frame");
    }
    // The handle is copied into the lambda, not referenced from it: what makes shared
    // ownership worth having is that the pixels survive this call, and a lambda capturing
    // the caller's TimedFrame by reference would give that back on the first line.
    Frame held = frame.frame;
    return m_impl->submit(frame.metadata, [&](const std::vector<Rect>& rects, int64_t pts,
                                              std::span<const uint8_t> sei) {
        m_impl->encoder.encode_buffer(held.mutable_buffer(), rects, pts, sei);
    });
}

Status SmartVideoEncoder::Impl::submit(const FrameMetadata& metadata, const Handover& hand)
{
    Impl& s = *this;
    if (s.state == EncoderState::Failed) {
        return s.fatal;
    }
    if (s.state != EncoderState::Configured && s.state != EncoderState::Running) {
        return Status::InvalidArgument(
            std::string("SubmitFrame in state ") + encoder_state_name(s.state));
    }
    if (s.queue_full()) {
        // Backpressure by §24: a code, not a block. The caller drains and comes back.
        return Status::Again();
    }

    s.state = EncoderState::Running;
    const int64_t pts = metadata.timestamp.valid() ? metadata.timestamp.value : 0;

    try {
        std::vector<Rect> rects;
        if (const Status made = s.rects_for(metadata, &rects); !made.ok()) {
            return made;
        }
        if (!s.warned_roi && s.encoder.kind() == Backend::NvidiaNvenc) {
            s.log.warn("warning: stock FFmpeg hevc_nvenc ignores ROI; "
                       "qpDeltaMap is built but not submitted to NVENC SDK");
            s.warned_roi = true;
        }
        std::vector<uint8_t> sei;
        if (const Status made = s.metadata_for(metadata, pts, &sei); !made.ok()) {
            return made;
        }
        if (metadata.force_key_frame || s.key_frame_pending) {
            ++s.stats.keyframes_requested;
            s.key_frame_pending = false;
        }
        hand(rects, pts, sei);
    } catch (const std::exception& e) {
        s.fatal = Status::BackendError(e.what());
        s.state = EncoderState::Failed;
        return s.fatal;
    }

    ++s.stats.frames_submitted;
    s.stats.frames_converted = s.encoder.frames_converted();
    s.stats.frames_uploaded = s.encoder.frames_uploaded();
    // After the frame, not before it: whether the map fitted the codec's scale is
    // something only the backend that applied it can say.
    s.stats.frames_quantiser_clamped = s.encoder.frames_quantiser_clamped();
    if (s.stats.frames_quantiser_clamped > 0) {
        s.count_fallback(s.warned_qp_clamped);
        guard_quantiser_clamped(s.stats.frames_quantiser_clamped, s.log, s.warned_qp_clamped);
    }
    return Status::Ok();
}

Status SmartVideoEncoder::SubmitFrame(IFrameBuffer& pixels, const FrameMetadata& metadata)
{
    return m_impl->submit(metadata, [&](const std::vector<Rect>& rects, int64_t pts,
                                        std::span<const uint8_t> sei) {
        m_impl->encoder.encode_buffer(pixels, rects, pts, sei);
    });
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
    // §29's "encoder reset". Said at Info because it is the end of an encode and the
    // counts beside it are what a reader wants when the next line is from a different
    // encoder: how much came out, and how much of it was degraded.
    s.log.info(std::format(
        "encoder stopped from {}: {} frames in, {} packets out, {} bytes, "
        "{} fallback(s), {} region(s) dropped",
        encoder_state_name(s.state), s.stats.frames_submitted, s.stats.packets_received,
        s.stats.bytes_encoded, s.stats.fallbacks_applied, s.stats.regions_dropped));
    // §19.4's report on the records that never got a packet. It lives in the sink's end(),
    // and until 2026-09-04 nothing on this path called it: the counter was kept and thrown
    // away, so a sidecar that had stopped corresponding to the stream said nothing at all.
    // Stop is where it belongs: §8.13 makes it the end of an encode. A caller who simply
    // destroys the encoder still gets nothing, and that is deliberate -- warning from a
    // destructor is a worse trade than a caller who skipped the documented ending.
    s.end();
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
        if (!s.encoder.capabilities().native_bitrate && s.out_codec == Codec::H265
            && rc.target_bitrate > 0) {
            // Same guard as at Create, and it answers rather than quietly ignores: the
            // caller is asking now, so Permissive would still be misleading here.
            return guard_cpu_h265_target(true, FallbackPolicy::Strict, s.log);
        }
        s.config.settings.rate_control = rc;
        // The budget is an outer loop over the encoder's constant quantizer, so a new
        // target takes effect from the next frame without reopening anything -- and it is
        // left switched off under a backend that runs its own, for the reason spelled out
        // at Create.
        s.bitrate = BitrateBudget(
            outer_loop_policy(rc, s.encoder.capabilities().native_bitrate));
    }
    // §29's "reconfigure", and it names what actually changed rather than that a call
    // happened: §8.2 lets three things move and a caller may set any subset, so "reconfigured"
    // on its own would leave a reader to guess which.
    //
    // A call that set nothing says nothing. It is Ok -- there was nothing invalid about it --
    // but no setting moved, and a bare "reconfigured:" naming no field would put a
    // reconfiguration in the log that did not happen, which is the opposite of what §29
    // wants the event for.
    if (settings.frame_rate || settings.quality || settings.target_bitrate) {
        s.log.info(std::format("reconfigured:{}{}{}",
            settings.frame_rate ? " frame_rate" : "", settings.quality ? " quality" : "",
            settings.target_bitrate ? " target_bitrate" : ""));
    }
    return Status::Ok();
}

Status SmartVideoEncoder::GetCodecParameters(CodecParameters* parameters) const
{
    if (parameters == nullptr) {
        return Status::InvalidArgument("GetCodecParameters needs somewhere to write");
    }
    if (m_impl->state == EncoderState::Created) {
        return Status::InvalidArgument("no encoder is open");
    }
    // Read once at open from whichever backend answered, so this no longer needs a codec
    // context and works for a backend that has none.
    *parameters = m_impl->encoder.codec_parameters();
    return Status::Ok();
}

EncoderCapabilities SmartVideoEncoder::GetCapabilities() const
{
    return m_impl->encoder.capabilities();
}

EffectiveSettings SmartVideoEncoder::GetEffectiveSettings() const
{
    return m_impl->encoder.effective_settings();
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
    r.backend_name = s.backend_name;
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
