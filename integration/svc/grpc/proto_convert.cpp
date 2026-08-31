#include "grpc/proto_convert.h"

#include "grpc/path_guard.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace roisvc {


// AVPixelFormat to the library's own PixelFormat. The mapping lives on this side because
// the wire format is ta_vms' contract with this service, not the library's -- the library
// deliberately does not know FFmpeg's numbering, and this is the edge where that stops
// mattering.
sve::PixelFormat to_library_format(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return sve::PixelFormat::YUV420P;
    case AV_PIX_FMT_NV12:    return sve::PixelFormat::NV12;
    case AV_PIX_FMT_YUV422P: return sve::PixelFormat::YUV422P;
    case AV_PIX_FMT_YUV444P: return sve::PixelFormat::YUV444P;
    case AV_PIX_FMT_GRAY8:   return sve::PixelFormat::GRAY8;
    default:                 return sve::PixelFormat::Unknown;
    }
}

sve::Preset to_library_preset(roi::transcode::Preset p)
{
    switch (p) {
    case roi::transcode::PRESET_FASTEST: return sve::Preset::Fastest;
    case roi::transcode::PRESET_FAST:    return sve::Preset::Fast;
    case roi::transcode::PRESET_SLOW:    return sve::Preset::Slow;
    case roi::transcode::PRESET_SLOWEST: return sve::Preset::Slowest;
    default:                             return sve::Preset::Balanced;
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

sve::BackendChoice from_proto(roi::transcode::Encoder e)
{
    switch (e) {
    case roi::transcode::ENCODER_QSV:
        return sve::backend_choice_from_name("qsv");
    case roi::transcode::ENCODER_VAAPI:
        return sve::backend_choice_from_name("vaapi");
    case roi::transcode::ENCODER_NVENC:
        return sve::backend_choice_from_name("nvenc");
    case roi::transcode::ENCODER_CPU:
        // Still one value on the wire, and it stays one: 4.1 split the software backend in
        // two inside the library, which is not a reason to break every client that already
        // sends this. It resolves to Auto over x264 and x265, so whichever of the two
        // cannot produce the requested codec is skipped with its reason logged.
        return sve::backend_choice_from_name("cpu");
    default:
        return sve::backend_choice_from_name("auto");
    }
}

roi::transcode::Encoder to_proto(sve::Backend k)
{
    switch (k) {
    case sve::Backend::IntelOneVpl:
        return roi::transcode::ENCODER_QSV;
    case sve::Backend::Vaapi:
        return roi::transcode::ENCODER_VAAPI;
    case sve::Backend::NvidiaNvenc:
        return roi::transcode::ENCODER_NVENC;
    case sve::Backend::X264:
    case sve::Backend::X265:
        return roi::transcode::ENCODER_CPU;
    default:
        return roi::transcode::ENCODER_AUTO;
    }
}

// Backend knobs are shared by both entry points, so unpacking them is too.
// One Settings serves both entry points, so unpacking it is written once.
// FileSettings and StreamSettings are distinct types with identical fields, so
// one template serves both and they cannot drift apart by accident.
sve::ObjectPriority priority_from_proto(roi::transcode::ObjectPriority p)
{
    switch (p) {
        case roi::transcode::OBJECT_PRIORITY_BACKGROUND:
            return sve::ObjectPriority::Background;
        case roi::transcode::OBJECT_PRIORITY_LOW:
            return sve::ObjectPriority::Low;
        case roi::transcode::OBJECT_PRIORITY_HIGH:
            return sve::ObjectPriority::High;
        case roi::transcode::OBJECT_PRIORITY_CRITICAL:
            return sve::ObjectPriority::Critical;
        default:
            return sve::ObjectPriority::Normal;
    }
}

sve::Box box_from_proto(const roi::transcode::Box& b)
{
    sve::Box out;
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


sve::InputCodec codec_from_proto(roi::transcode::InputCodec c)
{
    switch (c) {
    case roi::transcode::INPUT_CODEC_H264:
        return sve::InputCodec::H264;
    case roi::transcode::INPUT_CODEC_H265:
        return sve::InputCodec::H265;
    case roi::transcode::INPUT_CODEC_JPEG:
        return sve::InputCodec::Jpeg;
    default:
        return sve::InputCodec::Raw;
    }
}

sve::SessionParams params_from_open(const roi::transcode::StreamOpen& open)
{
    sve::SessionParams sp;
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


} // namespace roisvc
