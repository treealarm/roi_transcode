#include "grpc/roi_transcode_service.h"

#include "grpc/path_guard.h"
#include "job/roi_job_manager.h"
#include "roi_json.h"

#include <chrono>
#include <cstring>
#include <format>

namespace {
int64_t to_unix_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch())
        .count();
}

roi::transcode::RoiTranscodeState to_proto(RoiJobState s)
{
    switch (s) {
    case RoiJobState::Running:
        return roi::transcode::ROI_TRANSCODE_STATE_RUNNING;
    case RoiJobState::Completed:
        return roi::transcode::ROI_TRANSCODE_STATE_COMPLETED;
    case RoiJobState::Failed:
        return roi::transcode::ROI_TRANSCODE_STATE_FAILED;
    case RoiJobState::Cancelled:
        return roi::transcode::ROI_TRANSCODE_STATE_CANCELLED;
    }
    return roi::transcode::ROI_TRANSCODE_STATE_UNSPECIFIED;
}

void fill_job(const RoiJobStatus& st, roi::transcode::RoiTranscodeJob* out)
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
}

EncoderKind parse_encoder(const std::string& s)
{
    if (s.empty() || s == "auto") {
        return EncoderKind::Auto;
    }
    if (s == "qsv") {
        return EncoderKind::Qsv;
    }
    if (s == "vaapi") {
        return EncoderKind::Vaapi;
    }
    if (s == "nvenc") {
        return EncoderKind::Nvenc;
    }
    if (s == "x265") {
        return EncoderKind::X265;
    }
    throw std::runtime_error(
        "unknown encoder (auto|qsv|vaapi|nvenc|x265): " + s);
}

RoiJobRequest build_request(const roi::transcode::RoiTranscodeRequest& req)
{
    RoiJobRequest out;
    out.options.input_path = require_allowed_path(req.input_path(), "input_path");
    out.options.output_path = require_allowed_path(req.output_path(), "output_path");
    out.options.encoder = parse_encoder(req.encoder());
    if (req.crf() > 0) {
        out.options.crf = req.crf();
    }
    if (req.gop() > 0) {
        out.options.gop = req.gop();
    }
    if (!req.preset().empty()) {
        out.options.preset = req.preset();
    }

    if (!req.roi_json_path().empty()) {
        out.options.roi = load_roi_json(
            require_allowed_path(req.roi_json_path(), "roi_json_path"));
        return out;
    }

    RoiDocument doc;
    if (req.has_background_qp_delta()) {
        doc.background_qp_delta = req.background_qp_delta();
    }
    if (req.has_hold_sec() && req.hold_sec() > 0) {
        doc.hold_sec = req.hold_sec();
    }
    for (const auto& k : req.kinds()) {
        KindConfig cfg;
        if (k.has_qp_delta()) {
            cfg.qp_delta = k.qp_delta();
        }
        if (k.has_pad_norm() && k.pad_norm() > 0) {
            cfg.pad_norm = k.pad_norm();
        }
        doc.kinds.emplace(k.kind(), cfg);
    }
    for (const auto& b : req.boxes()) {
        RoiSample s;
        s.t = b.t();
        s.kind = b.kind();
        s.bbox_x = b.bbox_x();
        s.bbox_y = b.bbox_y();
        s.bbox_w = b.bbox_w();
        s.bbox_h = b.bbox_h();
        doc.samples.push_back(s);
    }
    out.options.roi = std::move(doc);
    return out;
}
} // namespace

grpc::Status RoiTranscodeServiceImpl::StartRoiTranscode(
    grpc::ServerContext*,
    const roi::transcode::RoiTranscodeRequest* request,
    roi::transcode::RoiTranscodeJob* response)
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

grpc::Status RoiTranscodeServiceImpl::GetRoiTranscodeJob(
    grpc::ServerContext*,
    const roi::transcode::RoiTranscodeJobKey* request,
    roi::transcode::RoiTranscodeJob* response)
{
    const auto st = RoiJobManager::instance().get(request->job_id());
    if (!st) {
        return {grpc::StatusCode::NOT_FOUND, "unknown job"};
    }
    fill_job(*st, response);
    return grpc::Status::OK;
}

grpc::Status RoiTranscodeServiceImpl::CancelRoiTranscode(
    grpc::ServerContext*,
    const roi::transcode::RoiTranscodeJobKey* request,
    roi::transcode::OperationStatus* response)
{
    const bool ok = RoiJobManager::instance().cancel(request->job_id());
    response->set_success(ok);
    response->set_message(ok ? "cancelling" : "job unknown or already finished");
    return grpc::Status::OK;
}

grpc::Status RoiTranscodeServiceImpl::ListRoiTranscodeJobs(
    grpc::ServerContext*,
    const roi::transcode::ListRoiTranscodeJobsRequest*,
    roi::transcode::ListRoiTranscodeJobsResponse* response)
{
    for (const auto& st : RoiJobManager::instance().list()) {
        fill_job(st, response->add_jobs());
    }
    return grpc::Status::OK;
}
