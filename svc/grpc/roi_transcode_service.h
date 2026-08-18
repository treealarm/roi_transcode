#pragma once

#include "job/roi_job_manager.h"
#include "roi_transcode.grpc.pb.h"

#include <grpcpp/grpcpp.h>

class RoiTranscodeServiceImpl final
    : public roi::transcode::RoiTranscodeService::Service {
public:
    grpc::Status StartRoiTranscode(
        grpc::ServerContext* context,
        const roi::transcode::RoiTranscodeRequest* request,
        roi::transcode::RoiTranscodeJob* response) override;

    grpc::Status GetRoiTranscodeJob(
        grpc::ServerContext* context,
        const roi::transcode::RoiTranscodeJobKey* request,
        roi::transcode::RoiTranscodeJob* response) override;

    grpc::Status CancelRoiTranscode(
        grpc::ServerContext* context,
        const roi::transcode::RoiTranscodeJobKey* request,
        roi::transcode::OperationStatus* response) override;

    grpc::Status ListRoiTranscodeJobs(
        grpc::ServerContext* context,
        const roi::transcode::ListRoiTranscodeJobsRequest* request,
        roi::transcode::ListRoiTranscodeJobsResponse* response) override;
};
