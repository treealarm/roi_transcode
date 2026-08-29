#include "grpc/roi_transcode_service.h"

#include "grpc/path_guard.h"
#include "job/roi_job_manager.h"
#include "smartvideo/session.h"

#include <chrono>
#include <cstring>
#include <span>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace {

// AVPixelFormat to the library's own PixelFormat. The mapping lives on this side because
// the wire format is ta_vms' contract with this service, not the library's -- the library
// deliberately does not know FFmpeg's numbering, and this is the edge where that stops
// mattering.
smartvideo::PixelFormat to_library_format(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return smartvideo::PixelFormat::YUV420P;
    case AV_PIX_FMT_NV12:    return smartvideo::PixelFormat::NV12;
    case AV_PIX_FMT_YUV422P: return smartvideo::PixelFormat::YUV422P;
    case AV_PIX_FMT_YUV444P: return smartvideo::PixelFormat::YUV444P;
    case AV_PIX_FMT_GRAY8:   return smartvideo::PixelFormat::GRAY8;
    default:                 return smartvideo::PixelFormat::Unknown;
    }
}

smartvideo::Preset to_library_preset(roi::transcode::Preset p)
{
    switch (p) {
    case roi::transcode::PRESET_FASTEST: return smartvideo::Preset::Fastest;
    case roi::transcode::PRESET_FAST:    return smartvideo::Preset::Fast;
    case roi::transcode::PRESET_SLOW:    return smartvideo::Preset::Slow;
    case roi::transcode::PRESET_SLOWEST: return smartvideo::Preset::Slowest;
    default:                             return smartvideo::Preset::Balanced;
    }
}

int64_t to_unix_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch())
        .count();
}

roi::transcode::JobState to_proto(RoiJobState s)
{
    switch (s) {
    case RoiJobState::Running:
        return roi::transcode::JOB_STATE_RUNNING;
    case RoiJobState::Completed:
        return roi::transcode::JOB_STATE_COMPLETED;
    case RoiJobState::Failed:
        return roi::transcode::JOB_STATE_FAILED;
    case RoiJobState::Cancelled:
        return roi::transcode::JOB_STATE_CANCELLED;
    }
    return roi::transcode::JOB_STATE_UNSPECIFIED;
}

void fill_lag(const bool measured, const int64_t samples, const float min_iou,
    const double p95_frames, const double p99_frames, const double p95_ms,
    const double p99_ms, const bool acceptance, roi::transcode::RoiLagStats* out)
{
    out->set_measured(measured);
    out->set_samples(samples);
    out->set_min_iou(min_iou);
    out->set_p95_frames(p95_frames);
    out->set_p99_frames(p99_frames);
    out->set_p95_ms(p95_ms);
    out->set_p99_ms(p99_ms);
    out->set_acceptance(acceptance);
}

void fill_job(const RoiJobStatus& st, roi::transcode::Job* out)
{
    out->set_job_id(st.job_id);
    out->set_input_path(st.input_path);
    out->set_output_path(st.output_path);
    out->set_state(to_proto(st.state));
    out->set_start_unix_ms(to_unix_ms(st.start_ts));
    if (st.end_ts.time_since_epoch().count() != 0) {
        out->set_end_unix_ms(to_unix_ms(st.end_ts));
    }
    out->set_message(st.message);
    out->set_encoder_used(st.encoder_used);
    out->set_cpu_h265_target_guard(st.cpu_h265_target_guard);
    fill_lag(st.roi_lag_measured, st.roi_lag_samples, st.roi_lag_min_iou,
        st.roi_lag_p95_frames, st.roi_lag_p99_frames, st.roi_lag_p95_ms,
        st.roi_lag_p99_ms, st.roi_lag_acceptance, out->mutable_roi_lag());
}

smartvideo::Encoder from_proto(roi::transcode::Encoder e)
{
    switch (e) {
    case roi::transcode::ENCODER_QSV:
        return smartvideo::Encoder::Qsv;
    case roi::transcode::ENCODER_VAAPI:
        return smartvideo::Encoder::Vaapi;
    case roi::transcode::ENCODER_NVENC:
        return smartvideo::Encoder::Nvenc;
    case roi::transcode::ENCODER_CPU:
        return smartvideo::Encoder::Cpu;
    default:
        return smartvideo::Encoder::Auto;
    }
}

roi::transcode::Encoder to_proto(smartvideo::Encoder k)
{
    switch (k) {
    case smartvideo::Encoder::Qsv:
        return roi::transcode::ENCODER_QSV;
    case smartvideo::Encoder::Vaapi:
        return roi::transcode::ENCODER_VAAPI;
    case smartvideo::Encoder::Nvenc:
        return roi::transcode::ENCODER_NVENC;
    case smartvideo::Encoder::Cpu:
        return roi::transcode::ENCODER_CPU;
    default:
        return roi::transcode::ENCODER_AUTO;
    }
}

// Encoder knobs are shared by both entry points, so unpacking them is too.
// One Settings serves both entry points, so unpacking it is written once.
// FileSettings and StreamSettings are distinct types with identical fields, so
// one template serves both and they cannot drift apart by accident.
template <typename ProtoSettings>
void apply_settings(const ProtoSettings& in, smartvideo::Settings& out)
{
    out.encoder = from_proto(in.encoder());
    switch (in.codec()) {
        case roi::transcode::CODEC_H264: out.codec = smartvideo::Codec::H264; break;
        case roi::transcode::CODEC_SAME_AS_SOURCE:
            out.codec = smartvideo::Codec::SameAsSource; break;
        default: out.codec = smartvideo::Codec::H265; break;
    }
    if (in.crf() > 0) {
        out.crf = in.crf();
    }
    if (in.gop() > 0) {
        out.gop = in.gop();
    }
    out.preset = to_library_preset(in.preset());

    if (in.has_rate_control()) {
        const auto& rc = in.rate_control();
        if (rc.target_bitrate() > 0) {
            out.rate_control.target_bitrate = rc.target_bitrate();
        }
        if (rc.has_max_average_overshoot()) {
            out.rate_control.max_average_overshoot = rc.max_average_overshoot();
        }
        if (rc.has_critical_importance()) {
            out.rate_control.critical_importance = rc.critical_importance();
        }
        if (rc.has_max_peak_overshoot()) {
            out.rate_control.max_peak_overshoot = rc.max_peak_overshoot();
        }
        if (rc.has_recovery_window_ms()) {
            out.rate_control.recovery_window_ms = rc.recovery_window_ms();
        }
    }

    const auto& q = in.roi();
    if (q.has_background_qp_delta()) {
        out.quality.background_qp_delta = q.background_qp_delta();
    }
    if (q.has_hold_sec() && q.hold_sec() > 0) {
        out.quality.hold_sec = q.hold_sec();
    }
    if (q.max_regions() > 0) {
        out.quality.max_regions = q.max_regions();
    }
    for (const auto& k : q.kinds()) {
        smartvideo::KindQuality cfg;
        if (k.has_qp_delta()) {
            cfg.qp_delta = k.qp_delta();
        }
        if (k.has_pad() && k.pad() > 0) {
            cfg.pad = k.pad();
        }
        out.quality.kinds.emplace(k.kind(), cfg);
    }
    if (q.has_mask_levels() && q.mask_levels() > 0) {
        out.quality.mask_levels = q.mask_levels();
    }
    if (q.has_mask_qp_delta_at_255()) {
        out.quality.mask_qp_delta_at_255 = q.mask_qp_delta_at_255();
    } else {
        // Not asked for: a mask carries one strength, so it takes the strongest
        // kind on offer. Sending boxes and a mask with consistent settings is
        // then one less thing to get right.
        for (const auto& [name, cfg] : out.quality.kinds) {
            (void)name;
            out.quality.mask_qp_delta_at_255 =
                std::min(out.quality.mask_qp_delta_at_255, cfg.qp_delta);
        }
    }
    if (q.has_pad() && q.pad() > 0) {
        out.quality.pad = q.pad();
    }
    if (q.has_max_protected_fraction()) {
        out.quality.max_protected_fraction = q.max_protected_fraction();
    }
    switch (q.margin_mode()) {
        case roi::transcode::MARGIN_MODE_FRACTION_OF_FRAME:
            out.quality.margin_mode = smartvideo::MarginMode::FractionOfFrame;
            break;
        default:
            out.quality.margin_mode = smartvideo::MarginMode::FixedPixels;
            break;
    }
    if (q.has_margin_px()) {
        out.quality.margin_px = q.margin_px();
    }
    if (q.has_min_roi_side()) {
        out.quality.min_roi_side = q.min_roi_side();
    }
    if (q.has_class_weighting()) {
        const auto& p = q.class_weighting();
        auto& dst = out.quality.importance;
        for (const auto& [k, v] : p.semantic_priority()) {
            dst.semantic_priority[k] = v;
        }
        for (const auto& [k, v] : p.task_priority()) {
            dst.task_priority[k] = v;
        }
        if (p.has_unknown_priority()) {
            dst.unknown_priority = p.unknown_priority();
        }
        if (p.has_assumed_confidence()) {
            dst.assumed_confidence = p.assumed_confidence();
        }
        if (p.has_weigh_confidence_on_known_classes()) {
            dst.weigh_confidence_on_known_classes = p.weigh_confidence_on_known_classes();
        }
        if (p.has_temporal_stability_floor()) {
            dst.temporal_stability_floor = p.temporal_stability_floor();
        }
        if (p.has_temporal_stability_tau_sec()) {
            dst.temporal_stability_tau_sec = p.temporal_stability_tau_sec();
        }
        if (p.has_use_motion_weight()) {
            dst.use_motion_weight = p.use_motion_weight();
        }
        if (p.has_motion_weight_floor()) {
            dst.motion_weight_floor = p.motion_weight_floor();
        }
        if (p.has_motion_speed_at_full()) {
            dst.motion_speed_at_full = p.motion_speed_at_full();
        }
    }

    if (in.has_temporal_roi()) {
        const auto& t = in.temporal_roi();
        if (t.has_max_missing_frames()) {
            out.temporal.max_missing_frames = t.max_missing_frames();
        }
        if (t.has_max_hold_sec()) {
            out.temporal.max_hold_sec = t.max_hold_sec();
        }
        if (t.has_confidence_decay()) {
            out.temporal.confidence_decay = t.confidence_decay();
        }
        if (t.has_max_qp_delta_per_frame()) {
            out.temporal.max_qp_delta_per_frame = t.max_qp_delta_per_frame();
        }
        if (t.has_spatial_smoothing()) {
            out.temporal.spatial_smoothing = t.spatial_smoothing();
        }
        if (t.has_temporal_smoothing()) {
            out.temporal.temporal_smoothing = t.temporal_smoothing();
        }
        if (t.has_use_track_prediction()) {
            out.temporal.use_track_prediction = t.use_track_prediction();
        }
    }
}

smartvideo::ObjectPriority priority_from_proto(roi::transcode::ObjectPriority p)
{
    switch (p) {
        case roi::transcode::OBJECT_PRIORITY_BACKGROUND:
            return smartvideo::ObjectPriority::Background;
        case roi::transcode::OBJECT_PRIORITY_LOW:
            return smartvideo::ObjectPriority::Low;
        case roi::transcode::OBJECT_PRIORITY_HIGH:
            return smartvideo::ObjectPriority::High;
        case roi::transcode::OBJECT_PRIORITY_CRITICAL:
            return smartvideo::ObjectPriority::Critical;
        default:
            return smartvideo::ObjectPriority::Normal;
    }
}

smartvideo::Box box_from_proto(const roi::transcode::Box& b)
{
    smartvideo::Box out;
    out.t_sec = b.t();
    out.kind = b.kind();
    out.x = b.x();
    out.y = b.y();
    out.width = b.width();
    out.height = b.height();
    out.track_id = static_cast<uint64_t>(b.track_id() > 0 ? b.track_id() : 0);
    out.confidence = b.confidence();
    out.label = b.label();
    out.polygon.reserve(static_cast<size_t>(b.polygon_size()));
    for (const auto& p : b.polygon()) {
        out.polygon.push_back({p.x(), p.y()});
    }
    if (b.has_priority()) {
        out.has_priority = true;
        out.priority = priority_from_proto(b.priority());
    }
    if (b.has_desired_qp_delta()) {
        out.has_desired_qp_delta = true;
        out.desired_qp_delta = b.desired_qp_delta();
    }
    return out;
}

RoiJobRequest build_request(const roi::transcode::FileJobRequest& req)
{
    RoiJobRequest out;
    out.input_path = require_allowed_path(req.input_path(), "input_path");
    out.output_path = require_allowed_path(req.output_path(), "output_path");
    apply_settings(req.settings(), out.settings);
    out.settings.movflags = req.movflags();

    for (const auto& b : req.boxes()) {
        out.boxes.push_back(box_from_proto(b));
    }
    return out;
}

} // namespace

grpc::Status RoiFileTranscodeServiceImpl::StartFileJob(
    grpc::ServerContext*,
    const roi::transcode::FileJobRequest* request,
    roi::transcode::Job* response)
{
    try {
        auto req = build_request(*request);
        std::string error;
        auto st = RoiJobManager::instance().start(std::move(req), error);
        if (!st) {
            return {grpc::StatusCode::INVALID_ARGUMENT, error};
        }
        fill_job(*st, response);
        return grpc::Status::OK;
    } catch (const std::exception& ex) {
        return {grpc::StatusCode::INVALID_ARGUMENT, ex.what()};
    }
}

grpc::Status RoiFileTranscodeServiceImpl::GetJob(
    grpc::ServerContext*,
    const roi::transcode::JobId* request,
    roi::transcode::Job* response)
{
    const auto st = RoiJobManager::instance().get(request->job_id());
    if (!st) {
        return {grpc::StatusCode::NOT_FOUND, "unknown job"};
    }
    fill_job(*st, response);
    return grpc::Status::OK;
}

grpc::Status RoiFileTranscodeServiceImpl::CancelJob(
    grpc::ServerContext*,
    const roi::transcode::JobId* request,
    roi::transcode::Job* response)
{
    // Returning the job rather than a bool: the caller wanted to know where the
    // job ended up, and "already COMPLETED" is a different answer from
    // "CANCELLED" that a success flag cannot carry.
    RoiJobManager::instance().cancel(request->job_id());
    const auto st = RoiJobManager::instance().get(request->job_id());
    if (!st) {
        return {grpc::StatusCode::NOT_FOUND, "unknown job"};
    }
    fill_job(*st, response);
    return grpc::Status::OK;
}

grpc::Status RoiFileTranscodeServiceImpl::ListJobs(
    grpc::ServerContext*,
    const roi::transcode::ListJobsRequest*,
    roi::transcode::ListJobsResponse* response)
{
    for (const auto& st : RoiJobManager::instance().list()) {
        fill_job(st, response->add_jobs());
    }
    return grpc::Status::OK;
}

namespace {

// Ships packets back over the stream. The alternative is output_path, in which
// case the session muxes and only the summary comes back.
class StreamSink : public smartvideo::PacketHandler {
public:
    using Writer = grpc::ServerReaderWriter<roi::transcode::EncodeStreamResponse,
        roi::transcode::EncodeStreamRequest>;

    explicit StreamSink(Writer* w) : m_writer(w) {}

    void on_open(const smartvideo::StreamInfo& info) override
    {
        roi::transcode::EncodeStreamResponse msg;
        auto* opened = msg.mutable_opened();
        opened->set_encoder_used(to_proto(info.encoder_used));
        opened->set_encoder_name(info.encoder_name);
        if (!info.extradata.empty()) {
            opened->set_extradata(info.extradata.data(), info.extradata.size());
        }
        m_writer->Write(msg);
    }

    void on_packet(const smartvideo::EncodedPacket& pkt) override
    {
        roi::transcode::EncodeStreamResponse msg;
        auto* out = msg.mutable_packet();
        out->set_pts(pkt.pts);
        out->set_dts(pkt.dts);
        out->set_key(pkt.key);
        out->set_data(pkt.data.data(), pkt.data.size());
        m_writer->Write(msg);
    }

private:
    Writer* m_writer;
};

smartvideo::InputCodec codec_from_proto(roi::transcode::InputCodec c)
{
    switch (c) {
    case roi::transcode::INPUT_CODEC_H264:
        return smartvideo::InputCodec::H264;
    case roi::transcode::INPUT_CODEC_H265:
        return smartvideo::InputCodec::H265;
    case roi::transcode::INPUT_CODEC_JPEG:
        return smartvideo::InputCodec::Jpeg;
    default:
        return smartvideo::InputCodec::Raw;
    }
}

smartvideo::SessionParams params_from_open(const roi::transcode::StreamOpen& open)
{
    smartvideo::SessionParams sp;
    sp.width = open.width();
    sp.height = open.height();
    if (open.time_base_num() > 0 && open.time_base_den() > 0) {
        sp.time_base_num = open.time_base_num();
        sp.time_base_den = open.time_base_den();
    }
    if (open.fps_num() > 0 && open.fps_den() > 0) {
        sp.fps_num = open.fps_num();
        sp.fps_den = open.fps_den();
    }
    sp.codec = codec_from_proto(open.codec());
    if (!open.extradata().empty()) {
        sp.extradata.assign(open.extradata().begin(), open.extradata().end());
    }
    apply_settings(open.settings(), sp.settings);
    sp.settings.movflags = open.movflags();
    return sp;
}

} // namespace

grpc::Status RoiStreamTranscodeServiceImpl::EncodeStream(grpc::ServerContext*,
    grpc::ServerReaderWriter<roi::transcode::EncodeStreamResponse,
        roi::transcode::EncodeStreamRequest>* stream)
{
    roi::transcode::EncodeStreamRequest req;
    if (!stream->Read(&req)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "empty stream"};
    }
    if (!req.has_open()) {
        return {grpc::StatusCode::INVALID_ARGUMENT,
            "the first message must be `open`"};
    }

    try {
        const auto& open = req.open();
        if (open.width() <= 0 || open.height() <= 0) {
            return {grpc::StatusCode::INVALID_ARGUMENT,
                "open needs a frame size"};
        }
        smartvideo::SessionParams sp = params_from_open(open);

        // Either the session muxes into a sandboxed path, or packets come back
        // over the stream. Opened is sent by the handler, on_open.
        std::unique_ptr<StreamSink> streaming;
        if (!open.output_path().empty()) {
            sp.output_path = require_allowed_path(open.output_path(), "output_path");
        } else {
            streaming = std::make_unique<StreamSink>(stream);
        }

        smartvideo::Session session(sp, streaming.get());

        // Raw planes arrive tightly packed, so the plane pointers are derived
        // rather than sent: one layout, described once in `open`.
        const auto pix = static_cast<AVPixelFormat>(open.pix_fmt());
        // The wire still carries an AVPixelFormat number -- that is ta_vms' own contract
        // with this service. The library has its own enum, so the translation happens here,
        // at the edge, rather than leaking FFmpeg's numbering into the library's surface.
        const smartvideo::PixelFormat frame_format =
            sp.codec == smartvideo::InputCodec::Raw
            ? to_library_format(pix)
            : smartvideo::PixelFormat::Unknown;
        if (sp.codec == smartvideo::InputCodec::Raw
            && frame_format == smartvideo::PixelFormat::Unknown) {
            return {grpc::StatusCode::INVALID_ARGUMENT,
                "pix_fmt names a format this encoder does not take"};
        }
        std::vector<smartvideo::Box> boxes;
        smartvideo::Mask mask;

        while (stream->Read(&req)) {
            if (!req.has_frame()) {
                return {grpc::StatusCode::INVALID_ARGUMENT,
                    "only `frame` may follow `open`"};
            }
            const auto& in = req.frame();

            smartvideo::Regions regions;
            boxes.clear();
            if (in.has_qp_map()) {
                const auto& m = in.qp_map();
                smartvideo::QpDeltaMap grid;
                grid.block = m.block();
                grid.cols = m.cols();
                grid.rows = m.rows();
                grid.width = m.width() > 0 ? m.width() : sp.width;
                grid.height = m.height() > 0 ? m.height() : sp.height;
                const auto& raw = m.deltas();
                grid.deltas.resize(raw.size());
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    grid.deltas[i] = static_cast<int8_t>(raw[i]);
                }
                const std::size_t expect =
                    static_cast<std::size_t>(std::max(grid.cols, 0))
                    * static_cast<std::size_t>(std::max(grid.rows, 0));
                if (grid.block <= 0 || grid.cols <= 0 || grid.rows <= 0
                    || grid.deltas.size() != expect) {
                    return {grpc::StatusCode::INVALID_ARGUMENT,
                        "qp_map deltas length must equal cols*rows with block>0"};
                }
                regions.qp_map = std::move(grid);
            } else if (in.has_mask()) {
                mask.data = std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(in.mask().data().data()),
                    in.mask().data().size());
                mask.width = in.mask().width();
                mask.height = in.mask().height();
                mask.stride = in.mask().width();
                regions.mask = mask;
            } else if (in.has_boxes()) {
                for (const auto& b : in.boxes().boxes()) {
                    boxes.push_back(box_from_proto(b));
                }
                regions.boxes = boxes;
            }

            const auto* bytes =
                reinterpret_cast<const uint8_t*>(in.data().data());
            if (sp.codec == smartvideo::InputCodec::Raw) {
                uint8_t* planes[4] = {nullptr, nullptr, nullptr, nullptr};
                int strides[4] = {0, 0, 0, 0};
                const int needed = av_image_fill_arrays(planes, strides,
                    bytes, pix, sp.width, sp.height, 1);
                if (needed < 0
                    || static_cast<size_t>(needed) > in.data().size()) {
                    return {grpc::StatusCode::INVALID_ARGUMENT,
                        "raw frame is smaller than the declared format needs"};
                }
                // Each plane's length is the distance to the next one, and for the last
                // the remainder of what the frame occupies. Derived here rather than
                // guessed inside the library, which is the point of PlaneBuffer asking.
                smartvideo::PlaneBuffer::Plane described[4];
                for (int i = 0; i < 4; ++i) {
                    if (!planes[i]) {
                        continue;
                    }
                    const uint8_t* next = nullptr;
                    for (int j = i + 1; j < 4 && !next; ++j) {
                        next = planes[j];
                    }
                    const std::size_t len = next
                        ? static_cast<std::size_t>(next - planes[i])
                        : static_cast<std::size_t>(needed)
                            - static_cast<std::size_t>(planes[i] - bytes);
                    described[i] = {std::span<uint8_t>(planes[i], len), strides[i]};
                }
                smartvideo::PlaneBuffer pixels(sp.width, sp.height, frame_format,
                    std::span<const smartvideo::PlaneBuffer::Plane>(described, 4));
                session.submit(pixels, regions, in.pts());
            } else {
                session.submit(
                    std::span<const uint8_t>(bytes, in.data().size()), regions,
                    in.pts());
            }
        }

        const smartvideo::Report rep = session.finish();
        roi::transcode::EncodeStreamResponse msg;
        auto* done = msg.mutable_done();
        done->set_frames(rep.frames);
        done->set_packets(rep.packets);
        done->set_bytes(rep.bytes);
        done->set_regions_dropped(rep.regions_dropped);
        done->set_cpu_h265_target_guard(rep.cpu_h265_target_guard);
        fill_lag(rep.roi_lag_measured, rep.roi_lag_samples, rep.roi_lag_min_iou,
            rep.roi_lag_p95_frames, rep.roi_lag_p99_frames, rep.roi_lag_p95_ms,
            rep.roi_lag_p99_ms, rep.roi_lag_acceptance, done->mutable_roi_lag());
        stream->Write(msg);
        return grpc::Status::OK;
    } catch (const std::exception& ex) {
        return {grpc::StatusCode::INVALID_ARGUMENT, ex.what()};
    }
}
