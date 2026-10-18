#include "ta_roienc/encode.h"

#include "bit_allowance.h"
#include "box_steadier.h"
#include "live_tuning_warnings.h"
#include "roi_picture_pipeline.h"
#include "weight_grid.h"
#include "journal.h"
#include "object_cleanup.h"
#include "access_unit_outlet.h"
#include "qp_rate_limit.h"
#include "transcoder_assembly.h"
#include "roi_coder.h"
#include "roi_delay.h"
#include "ta_roienc/metadata_file.h"

#include <format>
#include <deque>
#include <map>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace ta_roienc {

namespace {

/// The policy the outer loop should run under, given what the backend does for itself.
///
/// The outer loop exists for encoders that have no bitrate mode of their own. Where one
/// does, the target belongs to it: this returns the same policy with the target cleared,
/// which is how `BitAllowance` is told to stand down (`active()` is a non-zero target).
RateControlPolicy outer_loop_policy(const RateControlPolicy& policy, bool native_bitrate)
{
    if (!native_bitrate) {
        return policy;
    }
    RateControlPolicy off = policy;
    off.target_bitrate = 0;
    return off;
}

/// Depth of the packet queue between the backend and the caller -- bounded, and generous
/// rather than tight: a backend with lookahead can hand back a burst after a quiet stretch,
/// and the caller drains between frames anyway.
constexpr std::size_t kMaxQueuedPackets = 64;

} // namespace

const char* lifecycle_name(Lifecycle s)
{
    switch (s) {
    case Lifecycle::Created:     return "created";
    case Lifecycle::Configured:  return "configured";
    case Lifecycle::Running:     return "running";
    case Lifecycle::Draining:    return "draining";
    case Lifecycle::EndOfStream: return "end-of-stream";
    case Lifecycle::Stopped:     return "stopped";
    case Lifecycle::Failed:      return "failed";
    }
    return "?";
}

/// The four stateful collaborators plus the pipeline over them, in one place, so that the
/// file path and the session path cannot wire them differently.
struct VideoEncoder::Internals : AccessUnitOutlet {
    Internals(const EncodeOptions& cfg, const EngineCatalog& registry)
        : config(cfg)
        , encoder(registry)
    {
    }

    EncodeOptions config;
    EngineSetup backend_config;

    RoiCoder encoder;
    BitAllowance bitrate;
    QpRateLimiter qp_slew;
    BoxSmoother box_smooth;
    RoiGridBuilder roi_builder;

    std::deque<AccessUnit> queued;
    std::vector<RoiDelaySample> lag_samples;
    Counters stats;
    Lifecycle state = Lifecycle::Created;
    Verdict fatal;

    Journal log;
    EngineFallback policy = EngineFallback::Permissive;

    Coding out_codec = Coding::H265;
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

    // --- AccessUnitOutlet: where the backend's output lands until the caller asks for it. ---

    void begin(const CodingParams& params) override
    {
        out_codec = params.codec;
    }

    void on_access_unit(const AccessUnit& packet) override
    {
        if (bitrate.active()) {
            double dt = 0.0;
            const Ratio tb = packet.time_base;
            if (packet.duration > 0 && tb.den > 0) {
                dt = static_cast<double>(packet.duration) * tb.num / tb.den;
            }
            bitrate.observe(static_cast<int64_t>(packet.payload.size()),
                dt > 0.0 ? dt : frame_sec);
            // Said once, and counted, the moment the loop runs out of authority. A target
            // that cannot be reached because the expensive part of the frame is protected
            // is a degradation the caller is entitled to hear about.
            if (bitrate.target_unreachable() && !stats.bitrate_target_unreachable) {
                stats.bitrate_target_unreachable = true;
                ++stats.fallbacks_applied;
                log.warn("bitrate target unreachable: the outer loop is at its limit and "
                    "the stream is still above the target band -- the protected regions "
                    "carry more of the frame's cost than the background can pay back");
            }
        }

        AccessUnit out = packet;
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
        // adding them would count the same frame once on the way in and once on the way
        // out.
        const auto never_emitted = static_cast<int64_t>(pending_sidecar.size());
        pending_sidecar.clear();
        // Only once the stream was actually finished. Stop is safe from any state, so it is
        // also the cleanup path of a cancelled job -- and frames still inside a lookahead
        // when the caller gave up are not a broken sidecar, they are an encode nobody
        // drained. Warning there would report a data-integrity fault about a decision the
        // caller made on purpose.
        const bool stream_was_finished =
            state == Lifecycle::Draining || state == Lifecycle::EndOfStream;
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
        if (config.settings.metadata.sidecar == MetadataFile::None
            || !config.settings.metadata.metadata_file_sink) {
            return;
        }
        const auto it = pending_sidecar.find(pts);
        if (it == pending_sidecar.end()) {
            // A packet whose frame we never held. Not silently skipped: records are
            // promised to correspond to packets, and a missing one breaks that. Counted
            // separately from the frames that never produced a packet -- see end().
            ++sidecar_unmatched_packets;
            return;
        }
        config.settings.metadata.metadata_file_sink(write_metadata_ndjson(
            it->second, sidecar_index, config.settings.metadata.max_objects));
        ++sidecar_index;
        pending_sidecar.erase(it);
    }

    bool queue_full() const { return queued.size() >= kMaxQueuedPackets; }

    /// How the pixels reach the backend. The two public overloads differ in nothing but
    /// this call, so everything around it -- state checks, backpressure, regions, SEI,
    /// keyframe requests, failure handling -- is written once.
    using Handover = std::function<void(
        const std::vector<Area>& rects, int64_t pts, std::span<const uint8_t> sei)>;

    Verdict submit(const PictureInfo& metadata, const Handover& hand);

    /// ROI for one frame, plus the guards that must be audible when it degrades.
    Verdict rects_for(const PictureInfo& meta, std::vector<Area>* out_rects)
    {
        const EngineAbilities caps = encoder.capabilities();
        const bool have_map = !meta.regions.qp_offsets.deltas.empty();

        if (const Verdict s =
                guard_qp_map_quantized(have_map, caps, policy, log, warned_qp_map);
            !s.ok()) {
            return s;
        }
        if (have_map && !caps.qp_offsets) {
            count_fallback(warned_qp_map);
        }

        if (have_map) {
            int32_t dropped = 0;
            const int32_t budget = effective_region_budget(config.settings.quality, caps);
            *out_rects = areas_from_qp_offsets(meta.regions.qp_offsets, budget, &dropped);
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
        // Leaving this out is what the migration of Transcoder found: a caller passing a mask
        // got no regions at all, quietly, because the box path had nothing to work from.
        if (meta.regions.mask.consistent()) {
            const QualityPlan quality = bitrate.apply(config.settings.quality);
            MaskMapping mapping;
            mapping.qp_delta_at_0 = quality.background_qp_delta;
            mapping.qp_delta_at_255 = quality.mask_qp_delta_at_255;
            mapping.levels = quality.mask_levels;
            const int32_t block = effective_coding_block(caps);
            const QpOffsets grid = qp_map_from_mask(meta.regions.mask, mapping,
                config.input.width, config.input.height, block);
            int32_t dropped = 0;
            const int32_t budget = effective_region_budget(config.settings.quality, caps);
            *out_rects = areas_from_qp_offsets(grid, budget, &dropped);
            stats.regions_dropped += dropped;
            if (dropped > 0) {
                count_fallback(warned_roi_dropped);
            }
            last_block = block;
            log_roi_once(*out_rects, caps);
            return guard_roi_regions_dropped(
                stats.regions_dropped, caps, out_codec, policy, log, warned_roi_dropped);
        }

        RoiPicturePipelineInput in{
            .boxes = meta.regions.boxes,
            .quality = config.settings.quality,
            .temporal = config.settings.temporal,
            .caps = caps,
            .width = config.input.width,
            .height = config.input.height,
            .frame_interval_sec = frame_sec,
        };
        RoiPicturePipeline pipeline(bitrate, box_smooth, qp_slew, roi_builder);
        RoiPicturePipelineOutput out = pipeline.process(in);
        stats.regions_dropped += out.regions_dropped;
        if (out.regions_dropped > 0) {
            count_fallback(warned_roi_dropped);
        }
        if (const Verdict s = guard_roi_regions_dropped(stats.regions_dropped, caps,
                out_codec, policy, log, warned_roi_dropped);
            !s.ok()) {
            return s;
        }
        lag_samples.insert(
            lag_samples.end(), out.lag_samples.begin(), out.lag_samples.end());
        last_block = out.coding_block;
        log_roi_once(out.rects, caps);
        *out_rects = std::move(out.rects);
        return Verdict::Ok();
    }

    int32_t last_block = 16;

    /// One line describing the region map, on the first frame that has one -- the place
    /// to read the region count, which is how the per-codec Intel limit can be checked
    /// without a GPU-specific crash to look for.
    void log_roi_once(const std::vector<Area>& rects, const EngineAbilities& caps)
    {
        if (warned_roi_map) {
            return;
        }
        QpOffsetsBuilder builder;
        const auto grid =
            builder.build(config.input.width, config.input.height, last_block, rects);
        log.info(std::format("ROI: {} rects, qpDeltaMap {}x{} blocks of {}{}",
            rects.size(), grid.cols, grid.rows, last_block,
            caps.qp_offsets ? " (qp_map)" : ""));
        warned_roi_map = true;
    }
    bool warned_roi_map = false;

    /// Submitted metadata waiting for the packet it belongs to.
    ///
    /// Keyed by PTS because that is the only synchronisation key the packet carries back.
    /// Submission order is not emission order once a codec has lookahead or B-frames, so
    /// writing the record at submit would produce a file whose Nth line is not the Nth
    /// packet -- which is the one thing the sidecar promises.
    std::map<int64_t, SeiRecord> pending_sidecar;
    /// Position of the packet this record accompanies (the record's frame_index). Counts
    /// emissions, not submissions.
    uint64_t sidecar_index = 0;
    /// Packets that arrived under a timestamp no frame was submitted with.
    ///
    /// The mirror of `pending_sidecar` left non-empty at end(), and counted apart from it
    /// on purpose: one is a record with no packet, the other a packet with no record.
    int64_t sidecar_unmatched_packets = 0;

    /// The metadata for one frame, for both channels: the SEI payload and the sidecar.
    ///
    /// Built once and used twice on purpose. The sidecar exists precisely for the case
    /// where the bitstream cannot carry the objects, so it must not be reachable only
    /// through the SEI path, and deriving both from one SeiRecord keeps the two channels
    /// describing the same frame the same way.
    ///
    /// What goes in is the objects the caller submitted, not what the pipeline made of them
    /// after smoothing and normalisation: objects and the applied ROI are separate things.
    Verdict metadata_for(const PictureInfo& meta, int64_t pts, std::vector<uint8_t>* out)
    {
        const MetadataSetup& cfg = config.settings.metadata;
        const bool want_sidecar = cfg.sidecar != MetadataFile::None && cfg.metadata_file_sink;

        const EngineAbilities caps = encoder.capabilities();
        const bool wanted = cfg.bitstream_sei;
        if (wanted && !caps.sei) {
            count_fallback(warned_sei);
        }
        if (const Verdict s = guard_sei_unsupported(wanted, caps, policy, log, warned_sei);
            !s.ok()) {
            return s;
        }

        const bool want_sei = wanted && caps.sei;
        if (!want_sei && !want_sidecar) {
            return Verdict::Ok();
        }

        SeiRecord frame;
        frame.frame_id = static_cast<uint64_t>(stats.frames_submitted);
        frame.pts = pts;
        frame.objects = sei_objects_from_boxes(meta.regions.boxes, cfg.max_objects);
        if (want_sei) {
            *out = write_sei_side_data(frame, cfg.max_objects);
        }
        if (want_sidecar) {
            // Held until this frame's packet comes out; see write_sidecar. A second frame
            // at the same PTS replaces the first, which is the only sane reading of two
            // frames claiming one timestamp.
            pending_sidecar[pts] = std::move(frame);
        }
        return Verdict::Ok();
    }
};

VideoEncoder::VideoEncoder(std::unique_ptr<Internals> impl) : m_impl(std::move(impl)) {}
VideoEncoder::~VideoEncoder() = default;

std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncodeOptions& config)
{
    return CreateVideoEncoderIn(config, default_engine_catalog());
}

std::unique_ptr<VideoEncoder> CreateVideoEncoderIn(
    const EncodeOptions& config, const EngineCatalog& registry)
{
    if (!config.input.valid()) {
        throw EncodeError(Verdict::InvalidArgument("input video format is not usable"));
    }

    // A sidecar needs a sink. Refused here rather than accepted and dropped: a caller who
    // set the format and forgot the sink would otherwise get a successful encode and no
    // metadata.
    if (config.settings.metadata.sidecar != MetadataFile::None
        && !config.settings.metadata.metadata_file_sink) {
        throw EncodeError(Verdict::InvalidArgument(
            "a sidecar format was requested without a sidecar sink to write it to"));
    }

    auto impl = std::make_unique<VideoEncoder::Internals>(config, registry);
    impl->log = Journal(config.log);
    impl->policy = config.settings.fallback;

    EngineSetup& ep = impl->backend_config;
    ep.input = config.input;
    ep.engine_order = config.settings.engine_order;
    ep.kind = config.settings.encoder;
    ep.codec = config.settings.codec;
    ep.temporal = config.settings.temporal;
    ep.rate_control = config.settings.rate_control;
    ep.crf = config.settings.crf;
    ep.gop = config.settings.gop;
    ep.preset = config.settings.preset;

    // P010 needs Main10, and a profile that cannot carry the input's depth is refused here
    // rather than producing a file nothing will play.
    {
        const bool ten_bit = config.input.pixel_layout == PixelLayout::P010;
        const CodingProfile profile = config.settings.profile;
        if (ten_bit && profile != CodingProfile::Auto && profile != CodingProfile::H265Main10) {
            throw EncodeError(Verdict::InvalidArgument(std::string("P010 input needs the h265-main10 "
                "profile, not ") + coding_profile_name(profile)));
        }
        if (!ten_bit && profile == CodingProfile::H265Main10) {
            throw EncodeError(Verdict::InvalidArgument(
                "the h265-main10 profile needs ten-bit input; this one is eight"));
        }
        if (ten_bit && ep.codec == Coding::H264) {
            // The ten-bit profile here is HEVC's. H.264 has a ten-bit profile of its own,
            // but nothing here supports it, and claiming it would be inventing a contract.
            throw EncodeError(Verdict::InvalidArgument("ten-bit input is supported for H.265 only"));
        }
        ep.profile = profile;
    }

    // The preference list is the caller's to give, and an unusable one is refused here
    // rather than turned into a choice nobody asked for.
    if (ep.kind == Engine::Auto) {
        const std::vector<Engine>& want = ep.engine_order.preferred;
        if (want.empty()) {
            throw EncodeError(Verdict::InvalidArgument(
                "Engine::Auto needs an engine preference order; the list is empty"));
        }
        for (auto it = want.begin(); it != want.end(); ++it) {
            if (*it == Engine::Auto) {
                throw EncodeError(Verdict::InvalidArgument(
                    "Engine::Auto cannot appear inside its own preference order"));
            }
            if (std::find(want.begin(), it, *it) != it) {
                throw EncodeError(Verdict::InvalidArgument(
                    "the backend preference order lists the same backend twice"));
            }
        }
    }

    if (ep.codec == Coding::SameAsSource) {
        // No source to follow here: this entry point is handed frames, not a file. A caller
        // transcoding a clip resolves it against the input's own codec before Create.
        throw EncodeError(Verdict::InvalidArgument(
            "Coding::SameAsSource needs a source; resolve it before creating an encoder"));
    }

    // Named outright and unable to produce this codec -- X264 asked for HEVC, say -- is
    // Unsupported. Under Auto the same backend is merely skipped, which is why this check is
    // here and not in the chain walk.
    if (ep.kind != Engine::Auto && !engine_can(ep.kind, ep.codec, registry)) {
        throw EncodeError(Verdict::Unsupported(std::format("the {} backend cannot encode {}",
            engine_name(ep.kind), ep.codec == Coding::H264 ? "H.264" : "HEVC")));
    }

    if (config.input.frame_rate.num > 0 && config.input.frame_rate.den > 0) {
        impl->frame_sec = static_cast<double>(config.input.frame_rate.den)
            / static_cast<double>(config.input.frame_rate.num);
    }

    try {
        impl->encoder.open(ep, *impl);
    } catch (const EncodeError&) {
        throw;
    } catch (const std::exception& e) {
        // RoiCoder reports an exhausted chain by throwing, and its message already names
        // every backend that declined and why.
        throw EncodeError(Verdict::DeviceUnavailable(e.what()));
    }

    // The libx265 target-bitrate path is unstable, so it is refused rather than
    // half-honoured, and the refusal is both logged and counted.
    //
    // Asked of the backend that opened, not of the one that was requested: Auto on a
    // machine with no Quick Sync walks down to the software encoder on its own, and the
    // guard has to hold for every route in. Keyed on the capability rather than on which
    // backend it is, so that every HEVC software backend without a native bitrate mode is
    // caught -- a guard asking "who are you" where it means "what can you do" lets the next
    // such backend walk straight past it.
    if (!impl->encoder.capabilities().native_bitrate && ep.codec == Coding::H265
        && ep.rate_control.target_bitrate > 0) {
        if (const Verdict s = guard_cpu_h265_target(true, impl->policy, impl->log); !s.ok()) {
            throw EncodeError(s);
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
    // own, and its average went from 79% of target to 99%.
    impl->bitrate = BitAllowance(outer_loop_policy(ep.rate_control,
        impl->encoder.capabilities().native_bitrate));

    impl->stats.engine_used = impl->encoder.kind();
    impl->stats.engine_name = impl->encoder.label();
    impl->stats.backends_declined = impl->encoder.backends_declined();
    for (const std::string& reason : impl->encoder.backends_skipped()) {
        impl->log.warn("backend skipped: " + reason);
    }
    // The kind first, then the prose. The kind is what the caller selected and what a
    // script can read back; the label is a sentence for a person, and two backends can
    // legitimately start it with the same word -- x264 and x264direct both say "libx264",
    // which is true of both and useless for telling them apart.
    impl->log.info(std::format("backend: {} ({}) opened; {} declined before it",
        engine_name(impl->stats.engine_used), impl->encoder.label(),
        impl->stats.backends_declined));

    // The "capability report" event. It is the answer every fallback below is decided
    // against, so a log that records the fallbacks and not the capabilities they came from
    // is a log that cannot be read back to a cause. At Debug: it is one line per encode, not
    // per frame, but it is detail rather than news.
    {
        const EngineAbilities& c = impl->encoder.capabilities();
        impl->log.debug(std::format(
            "capabilities: regions={} roi_rect={} qp_map={} ({}x{}) ctu_qp={} "
            "emphasis={} runtime_roi={} sei={} native_bitrate={} main10={} p010={}",
            c.max_roi_regions, c.roi_areas, c.qp_offsets, c.qp_map_block_width,
            c.qp_map_block_height, c.ctu_qp, c.emphasis_map, c.runtime_roi_update, c.sei,
            c.native_bitrate, c.main10, c.p010_input));
    }

    impl->state = Lifecycle::Configured;
    return std::unique_ptr<VideoEncoder>(new VideoEncoder(std::move(impl)));
}

Verdict VideoEncoder::SubmitPicture(StampedPicture frame)
{
    if (!frame.frame) {
        return Verdict::InvalidArgument("SubmitPicture needs a frame");
    }
    // The handle is copied into the lambda, not referenced from it: what makes shared
    // ownership worth having is that the pixels survive this call, and a lambda capturing
    // the caller's StampedPicture by reference would give that back on the first line.
    Picture held = frame.frame;
    return m_impl->submit(frame.metadata, [&](const std::vector<Area>& rects, int64_t pts,
                                              std::span<const uint8_t> sei) {
        m_impl->encoder.encode_buffer(held.writable_data(), rects, pts, sei);
    });
}

Verdict VideoEncoder::Internals::submit(const PictureInfo& metadata, const Handover& hand)
{
    Internals& s = *this;
    if (s.state == Lifecycle::Failed) {
        return s.fatal;
    }
    if (s.state != Lifecycle::Configured && s.state != Lifecycle::Running) {
        return Verdict::InvalidArgument(
            std::string("SubmitPicture in state ") + lifecycle_name(s.state));
    }
    if (s.queue_full()) {
        // Backpressure: a code, not a block. The caller drains and comes back.
        return Verdict::Again();
    }

    s.state = Lifecycle::Running;
    const int64_t pts = metadata.timestamp.valid() ? metadata.timestamp.value : 0;

    try {
        std::vector<Area> rects;
        if (const Verdict made = s.rects_for(metadata, &rects); !made.ok()) {
            return made;
        }
        std::vector<uint8_t> sei;
        if (const Verdict made = s.metadata_for(metadata, pts, &sei); !made.ok()) {
            return made;
        }
        if (metadata.force_key_frame || s.key_frame_pending) {
            ++s.stats.keyframes_requested;
            s.key_frame_pending = false;
        }
        hand(rects, pts, sei);
    } catch (const std::exception& e) {
        s.fatal = Verdict::BackendError(e.what());
        s.state = Lifecycle::Failed;
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
    return Verdict::Ok();
}

Verdict VideoEncoder::SubmitPicture(IPictureStore& pixels, const PictureInfo& metadata)
{
    return m_impl->submit(metadata, [&](const std::vector<Area>& rects, int64_t pts,
                                        std::span<const uint8_t> sei) {
        m_impl->encoder.encode_buffer(pixels, rects, pts, sei);
    });
}

Verdict VideoEncoder::ReceiveAccessUnit(AccessUnit* packet)
{
    Internals& s = *m_impl;
    if (packet == nullptr) {
        return Verdict::InvalidArgument("ReceiveAccessUnit needs somewhere to put the packet");
    }
    if (!s.queued.empty()) {
        *packet = std::move(s.queued.front());
        s.queued.pop_front();
        return Verdict::Ok();
    }
    if (s.state == Lifecycle::Failed) {
        return s.fatal;
    }
    if (s.state == Lifecycle::Draining || s.state == Lifecycle::EndOfStream) {
        s.state = Lifecycle::EndOfStream;
        return Verdict::EndOfStream();
    }
    return Verdict::Again();
}

Verdict VideoEncoder::Flush()
{
    Internals& s = *m_impl;
    if (s.state == Lifecycle::Failed) {
        return s.fatal;
    }
    if (s.state == Lifecycle::Draining || s.state == Lifecycle::EndOfStream
        || s.state == Lifecycle::Stopped) {
        return Verdict::Ok();
    }
    try {
        s.encoder.flush();
    } catch (const std::exception& e) {
        s.fatal = Verdict::BackendError(e.what());
        s.state = Lifecycle::Failed;
        return s.fatal;
    }
    s.state = Lifecycle::Draining;
    return Verdict::Ok();
}

Verdict VideoEncoder::Stop()
{
    Internals& s = *m_impl;
    if (s.state == Lifecycle::Stopped) {
        return Verdict::Ok();
    }
    // The "encoder reset" event. Said at Info because it is the end of an encode and the
    // counts beside it are what a reader wants when the next line is from a different
    // encoder: how much came out, and how much of it was degraded.
    s.log.info(std::format(
        "encoder stopped from {}: {} frames in, {} packets out, {} bytes, "
        "{} fallback(s), {} region(s) dropped",
        lifecycle_name(s.state), s.stats.frames_submitted, s.stats.packets_received,
        s.stats.bytes_encoded, s.stats.fallbacks_applied, s.stats.regions_dropped));
    // The report on the records that never got a packet. It lives in the sink's end(), and
    // Stop is where it belongs: Stop is the end of an encode. A caller who simply destroys
    // the encoder still gets nothing, and that is deliberate -- warning from a destructor is
    // a worse trade than a caller who skipped the documented ending.
    s.end();
    s.queued.clear();
    s.state = Lifecycle::Stopped;
    return Verdict::Ok();
}

Verdict VideoEncoder::RequestIdr()
{
    Internals& s = *m_impl;
    if (s.state == Lifecycle::Failed) {
        return s.fatal;
    }
    if (s.state == Lifecycle::Stopped || s.state == Lifecycle::EndOfStream) {
        return Verdict::InvalidArgument(
            std::string("RequestIdr in state ") + lifecycle_name(s.state));
    }
    s.key_frame_pending = true;
    return Verdict::Ok();
}

Verdict VideoEncoder::Reconfigure(const LiveTuning& settings)
{
    Internals& s = *m_impl;
    if (s.state == Lifecycle::Failed) {
        return s.fatal;
    }
    if (s.state != Lifecycle::Configured && s.state != Lifecycle::Running) {
        return Verdict::InvalidArgument(
            std::string("Reconfigure in state ") + lifecycle_name(s.state));
    }

    if (settings.frame_rate) {
        const Ratio fps = *settings.frame_rate;
        if (fps.num <= 0 || fps.den <= 0) {
            return Verdict::InvalidArgument("frame rate must be positive");
        }
        s.frame_sec = static_cast<double>(fps.den) / static_cast<double>(fps.num);
    }
    if (settings.quality) {
        s.config.settings.quality = *settings.quality;
    }
    if (settings.target_bitrate) {
        RateControlPolicy rc = s.config.settings.rate_control;
        rc.target_bitrate = *settings.target_bitrate;
        if (!s.encoder.capabilities().native_bitrate && s.out_codec == Coding::H265
            && rc.target_bitrate > 0) {
            // Same guard as at Create, and it answers rather than quietly ignores: the
            // caller is asking now, so Permissive would still be misleading here.
            return guard_cpu_h265_target(true, EngineFallback::Strict, s.log);
        }
        s.config.settings.rate_control = rc;
        // The budget is an outer loop over the encoder's constant quantizer, so a new
        // target takes effect from the next frame without reopening anything -- and it is
        // left switched off under a backend that runs its own, for the reason spelled out
        // at Create.
        s.bitrate = BitAllowance(
            outer_loop_policy(rc, s.encoder.capabilities().native_bitrate));
    }
    // The "reconfigure" event, and it names what actually changed rather than that a call
    // happened: three things may move and a caller may set any subset, so "reconfigured" on
    // its own would leave a reader to guess which.
    //
    // A call that set nothing says nothing. It is Ok -- there was nothing invalid about it --
    // but no setting moved, and a bare "reconfigured:" naming no field would put a
    // reconfiguration in the log that did not happen.
    if (settings.frame_rate || settings.quality || settings.target_bitrate) {
        s.log.info(std::format("reconfigured:{}{}{}",
            settings.frame_rate ? " frame_rate" : "", settings.quality ? " quality" : "",
            settings.target_bitrate ? " target_bitrate" : ""));
    }
    return Verdict::Ok();
}

Verdict VideoEncoder::GetCodingParams(CodingParams* parameters) const
{
    if (parameters == nullptr) {
        return Verdict::InvalidArgument("GetCodingParams needs somewhere to write");
    }
    if (m_impl->state == Lifecycle::Created) {
        return Verdict::InvalidArgument("no encoder is open");
    }
    // Read once at open from whichever backend answered, so this needs no codec context
    // and works for a backend that has none.
    *parameters = m_impl->encoder.codec_parameters();
    return Verdict::Ok();
}

EngineAbilities VideoEncoder::GetCapabilities() const
{
    return m_impl->encoder.capabilities();
}

AppliedSettings VideoEncoder::GetEffectiveSettings() const
{
    return m_impl->encoder.effective_settings();
}

NativeHandle VideoEncoder::HardwareDevice() const
{
    return m_impl->encoder.hardware_device();
}

Lifecycle VideoEncoder::Phase() const { return m_impl->state; }

Counters VideoEncoder::GetStatistics() const
{
    Counters out = m_impl->stats;
    Outcome lag;
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

Outcome VideoEncoder::ToReport() const
{
    const Counters s = GetStatistics();
    Outcome r;
    r.engine_used = s.engine_used;
    r.engine_name = s.engine_name;
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

} // namespace ta_roienc
