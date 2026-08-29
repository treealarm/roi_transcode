// Exercises both entry points against a running roi-transcode-svc: the file job
// and the frame stream. The stream half demuxes a clip here and pushes access
// units over the wire, which is the closest thing to what an embedding project
// would do.

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "roi_json.h"
#include "roi_transcode.grpc.pb.h"

namespace rt = roi::transcode;

namespace {

int run_file_job(rt::RoiFileTranscode::Stub& stub, const std::string& in,
    const std::string& out, const std::string& roi_json)
{
    {
        grpc::ClientContext ctx;
        rt::ListJobsResponse resp;
        const auto st = stub.ListJobs(&ctx, rt::ListJobsRequest{}, &resp);
        if (!st.ok()) {
            std::cerr << "ListJobs failed: " << st.error_message() << "\n";
            return 1;
        }
        std::cout << "list ok jobs=" << resp.jobs_size() << std::endl;
    }

    rt::FileJobRequest req;
    req.set_input_path(in);
    req.set_output_path(out);
    auto* s = req.mutable_settings();
    // AUTO and H.264, deliberately. Forcing a backend makes this fail on any
    // machine that lacks it, for a reason it is not testing -- and the backend
    // that opened comes back in Job.encoder_used anyway, so a fall back to the
    // CPU is still visible, just as information rather than as a crash.
    s->set_encoder(rt::ENCODER_AUTO);
    s->set_codec(rt::CODEC_H264);
    s->set_crf(28);
    s->set_gop(1);

    // The service takes boxes and quality, never a path: reading the file is the
    // caller's job, which is what any integrator would be doing anyway.
    if (!roi_json.empty()) {
        const RoiJson doc = load_roi_json(roi_json);
        auto* q = s->mutable_roi();
        q->set_background_qp_delta(doc.quality.background_qp_delta);
        q->set_hold_sec(doc.quality.hold_sec);
        q->set_max_regions(doc.quality.max_regions);
        for (const auto& [kind, cfg] : doc.quality.kinds) {
            auto* k = q->add_kinds();
            k->set_kind(kind);
            k->set_qp_delta(cfg.qp_delta);
            k->set_pad(cfg.pad);
        }
        for (const auto& b : doc.boxes) {
            auto* pb = req.add_boxes();
            pb->set_t(b.t_sec);
            pb->set_kind(b.kind);
            pb->set_x(b.x);
            pb->set_y(b.y);
            pb->set_width(b.width);
            pb->set_height(b.height);
        }
    }

    rt::Job job;
    {
        grpc::ClientContext ctx;
        const auto st = stub.StartFileJob(&ctx, req, &job);
        if (!st.ok()) {
            std::cerr << "StartFileJob failed: " << st.error_message() << "\n";
            return 1;
        }
        std::cout << "started " << job.job_id() << std::endl;
    }

    for (int i = 0; i < 240; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        grpc::ClientContext ctx;
        rt::JobId key;
        key.set_job_id(job.job_id());
        rt::Job cur;
        const auto st = stub.GetJob(&ctx, key, &cur);
        if (!st.ok()) {
            std::cerr << "GetJob failed: " << st.error_message() << "\n";
            return 1;
        }
        if (cur.state() == rt::JOB_STATE_COMPLETED) {
            std::cout << "file job ok out=" << cur.output_path()
                      << " encoder=" << cur.encoder_used()
                      << " cpu_h265_guard=" << (cur.cpu_h265_target_guard() ? "true" : "false")
                      << " roi_lag.measured=" << (cur.roi_lag().measured() ? "true" : "false")
                      << " roi_lag.samples=" << cur.roi_lag().samples()
                      << std::endl;
            return 0;
        }
        if (cur.state() == rt::JOB_STATE_FAILED
            || cur.state() == rt::JOB_STATE_CANCELLED) {
            std::cerr << "job ended: " << cur.message() << "\n";
            return 2;
        }
    }
    std::cerr << "timeout\n";
    return 3;
}

int run_stream(rt::RoiStreamTranscode::Stub& stub, const std::string& in,
    const std::string& out)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, in.c_str(), nullptr, nullptr) < 0
        || avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "cannot open " << in << "\n";
        return 1;
    }
    int vindex = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vindex = static_cast<int>(i);
            break;
        }
    }
    if (vindex < 0) {
        std::cerr << "no video stream\n";
        avformat_close_input(&fmt);
        return 1;
    }
    AVStream* vs = fmt->streams[vindex];

    grpc::ClientContext ctx;
    auto stream = stub.EncodeStream(&ctx);

    rt::EncodeStreamRequest msg;
    auto* open = msg.mutable_open();
    open->set_width(vs->codecpar->width);
    open->set_height(vs->codecpar->height);
    open->set_time_base_num(vs->time_base.num);
    open->set_time_base_den(vs->time_base.den);
    open->set_fps_num(vs->avg_frame_rate.num);
    open->set_fps_den(vs->avg_frame_rate.den);
    open->set_codec(vs->codecpar->codec_id == AV_CODEC_ID_H264
            ? rt::INPUT_CODEC_H264
            : rt::INPUT_CODEC_H265);
    if (vs->codecpar->extradata_size > 0) {
        open->set_extradata(vs->codecpar->extradata,
            static_cast<size_t>(vs->codecpar->extradata_size));
    }
    open->set_output_path(out);
    auto* s = open->mutable_settings();
    s->set_encoder(rt::ENCODER_CPU);
    s->set_crf(28);
    s->set_gop(1);
    auto* q = s->mutable_roi();
    q->set_background_qp_delta(20);
    auto* k = q->add_kinds();
    k->set_kind("plate");
    k->set_qp_delta(-8);
    k->set_pad(0.01);
    if (!stream->Write(msg)) {
        std::cerr << "open rejected\n";
        avformat_close_input(&fmt);
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    int sent = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vindex) {
            rt::EncodeStreamRequest fmsg;
            auto* f = fmsg.mutable_frame();
            f->set_pts(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : 0);
            f->set_data(pkt->data, static_cast<size_t>(pkt->size));
            auto* box = f->mutable_boxes()->add_boxes();
            box->set_kind("plate");
            box->set_x(0.35f);
            box->set_y(0.4f);
            box->set_width(0.3f);
            box->set_height(0.2f);
            box->set_track_id(1);
            box->set_confidence(0.95f);
            if (!stream->Write(fmsg)) {
                break;
            }
            ++sent;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    stream->WritesDone();

    rt::EncodeStreamResponse resp;
    int packets = 0;
    while (stream->Read(&resp)) {
        if (resp.has_opened()) {
            std::cout << "stream opened encoder=" << resp.opened().encoder_name()
                      << std::endl;
        } else if (resp.has_packet()) {
            ++packets;
        } else if (resp.has_done()) {
            std::cout << "stream done frames=" << resp.done().frames()
                      << " packets=" << resp.done().packets()
                      << " bytes=" << resp.done().bytes()
                      << " cpu_h265_guard="
                      << (resp.done().cpu_h265_target_guard() ? "true" : "false")
                      << " roi_lag.measured="
                      << (resp.done().roi_lag().measured() ? "true" : "false")
                      << " roi_lag.samples=" << resp.done().roi_lag().samples()
                      << std::endl;
        }
    }
    const auto st = stream->Finish();
    if (!st.ok()) {
        std::cerr << "EncodeStream failed: " << st.error_message() << "\n";
        return 1;
    }
    std::cout << "sent " << sent << " frames, " << packets
              << " packets streamed back" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr
            << "Usage: roi_grpc_smoke HOST:PORT IN OUT [ROI.json] [--stream]\n"
               "  default mode runs a file job; --stream pushes frames.\n";
        return 2;
    }
    const std::string target = argv[1];
    const std::string in = argc > 2 ? argv[2] : "";
    const std::string out = argc > 3 ? argv[3] : "/tmp/roi-grpc-out.mp4";
    std::string roi_json;
    bool use_stream = false;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stream") == 0) {
            use_stream = true;
        } else {
            roi_json = argv[i];
        }
    }

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    if (use_stream) {
        auto stub = rt::RoiStreamTranscode::NewStub(channel);
        return run_stream(*stub, in, out);
    }
    auto stub = rt::RoiFileTranscode::NewStub(channel);
    return run_file_job(*stub, in, out, roi_json);
}
