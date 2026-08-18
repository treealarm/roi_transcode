#pragma once

#include "job/roi_job_manager.h"
#include "roi_transcode.grpc.pb.h"

#include <grpcpp/grpcpp.h>

class RoiTranscodeServiceImpl final
    : public roi::transcode::RoiTranscode::Service {
public:
    grpc::Status StartFileJob(
        grpc::ServerContext* context,
        const roi::transcode::FileJobRequest* request,
        roi::transcode::Job* response) override;

    grpc::Status GetJob(
        grpc::ServerContext* context,
        const roi::transcode::JobId* request,
        roi::transcode::Job* response) override;

    grpc::Status CancelJob(
        grpc::ServerContext* context,
        const roi::transcode::JobId* request,
        roi::transcode::Status* response) override;

    grpc::Status ListJobs(
        grpc::ServerContext* context,
        const roi::transcode::ListJobsRequest* request,
        roi::transcode::ListJobsResponse* response) override;

    grpc::Status EncodeStream(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<roi::transcode::StreamResponse,
            roi::transcode::StreamRequest>* stream) override;
};
