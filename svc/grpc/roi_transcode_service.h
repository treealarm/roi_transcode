#pragma once

#include "job/roi_job_manager.h"
#include "roi_transcode.grpc.pb.h"

#include <grpcpp/grpcpp.h>

// The two services are separate in the proto because they will diverge; they
// share a process because today they share a machine and a GPU.
class RoiFileTranscodeServiceImpl final
    : public roi::transcode::RoiFileTranscode::Service {
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
        roi::transcode::Job* response) override;

    grpc::Status ListJobs(
        grpc::ServerContext* context,
        const roi::transcode::ListJobsRequest* request,
        roi::transcode::ListJobsResponse* response) override;
};

class RoiStreamTranscodeServiceImpl final
    : public roi::transcode::RoiStreamTranscode::Service {
public:
    grpc::Status EncodeStream(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<roi::transcode::EncodeStreamResponse,
            roi::transcode::EncodeStreamRequest>* stream) override;
};
