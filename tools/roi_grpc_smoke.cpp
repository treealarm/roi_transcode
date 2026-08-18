#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "roi_transcode.grpc.pb.h"

int main(int argc, char** argv)
{
    const char* target = argc > 1 ? argv[1] : "127.0.0.1:50061";
    const char* in = argc > 2 ? argv[2]
                              : "/mnt/wdblue/tests/roi-transcode/testdata/synth.mp4";
    const char* out = argc > 3 ? argv[3] : "/tmp/roi-grpc-out.mp4";
    const char* roi = argc > 4 ? argv[4]
                               : "/mnt/wdblue/tests/roi-transcode/testdata/synth_roi.json";

    auto stub = roi::transcode::RoiTranscodeService::NewStub(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));

    {
        grpc::ClientContext ctx;
        roi::transcode::ListRoiTranscodeJobsResponse resp;
        const auto st = stub->ListRoiTranscodeJobs(
            &ctx, roi::transcode::ListRoiTranscodeJobsRequest{}, &resp);
        if (!st.ok()) {
            std::cerr << "List failed: " << st.error_message() << "\n";
            return 1;
        }
        std::cout << "list ok jobs=" << resp.jobs_size() << std::endl;
    }

    roi::transcode::RoiTranscodeRequest req;
    req.set_input_path(in);
    req.set_output_path(out);
    req.set_roi_json_path(roi);
    req.set_encoder("qsv");
    req.set_crf(28);
    req.set_gop(1);

    roi::transcode::RoiTranscodeJob job;
    {
        grpc::ClientContext ctx;
        const auto st = stub->StartRoiTranscode(&ctx, req, &job);
        if (!st.ok()) {
            std::cerr << "Start failed: " << st.error_message() << "\n";
            return 1;
        }
        std::cout << "started " << job.job_id() << " state=" << job.state()
                  << std::endl;
    }

    for (int i = 0; i < 80; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        grpc::ClientContext ctx;
        roi::transcode::RoiTranscodeJobKey key;
        key.set_job_id(job.job_id());
        roi::transcode::RoiTranscodeJob cur;
        const auto st = stub->GetRoiTranscodeJob(&ctx, key, &cur);
        if (!st.ok()) {
            std::cerr << "Get failed: " << st.error_message() << "\n";
            return 1;
        }
        std::cout << "poll state=" << cur.state() << " msg=" << cur.message()
                  << std::endl;
        if (cur.state() == roi::transcode::ROI_TRANSCODE_STATE_COMPLETED) {
            std::cout << "ok out=" << cur.output_path() << std::endl;
            return 0;
        }
        if (cur.state() == roi::transcode::ROI_TRANSCODE_STATE_FAILED
            || cur.state()
                == roi::transcode::ROI_TRANSCODE_STATE_CANCELLED) {
            std::cerr << "job ended: " << cur.message() << "\n";
            return 2;
        }
    }
    std::cerr << "timeout\n";
    return 3;
}
