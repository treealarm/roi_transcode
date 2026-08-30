#include "grpc/roi_transcode_service.h"

#include "grpc/path_guard.h"
#include "grpc/proto_convert.h"
#include "job/roi_job_manager.h"

#include <memory>

using namespace roisvc;

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
