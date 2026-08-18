#include "grpc/roi_transcode_service.h"

#include "grpc/path_guard.h"
#include "job/roi_job_manager.h"
#include "packet_sink.h"
#include "roi/session.h"
#include "roi_json.h"

#include <chrono>
#include <cstring>
#include <format>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace {
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
}

roi::Encoder from_proto(roi::transcode::Encoder e)
{
    switch (e) {
    case roi::transcode::ENCODER_QSV:
        return roi::Encoder::Qsv;
    case roi::transcode::ENCODER_VAAPI:
        return roi::Encoder::Vaapi;
    case roi::transcode::ENCODER_NVENC:
        return roi::Encoder::Nvenc;
    case roi::transcode::ENCODER_CPU:
        return roi::Encoder::Cpu;
    default:
        return roi::Encoder::Auto;
    }
}

roi::transcode::Encoder to_proto(roi::Encoder k)
{
    switch (k) {
    case roi::Encoder::Qsv:
        return roi::transcode::ENCODER_QSV;
    case roi::Encoder::Vaapi:
        return roi::transcode::ENCODER_VAAPI;
    case roi::Encoder::Nvenc:
        return roi::transcode::ENCODER_NVENC;
    case roi::Encoder::Cpu:
        return roi::transcode::ENCODER_CPU;
    default:
        return roi::transcode::ENCODER_AUTO;
    }
}

// Encoder knobs are shared by both entry points, so unpacking them is too.
// One Settings serves both entry points, so unpacking it is written once.
void apply_settings(const roi::transcode::Settings& in, roi::Settings& out)
{
    out.encoder = from_proto(in.encoder());
    out.codec = in.codec() == roi::transcode::CODEC_H264 ? roi::Codec::H264
                                                         : roi::Codec::H265;
    if (in.crf() > 0) {
        out.crf = in.crf();
    }
    if (in.gop() > 0) {
        out.gop = in.gop();
    }
    if (!in.preset().empty()) {
        out.preset = in.preset();
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
        roi::KindQuality cfg;
        if (k.has_qp_delta()) {
            cfg.qp_delta = k.qp_delta();
        }
        if (k.has_pad() && k.pad() > 0) {
            cfg.pad = k.pad();
        }
        out.quality.kinds.emplace(k.kind(), cfg);
    }
    // A mask carries one strength, so it takes the strongest kind on offer.
    for (const auto& [name, cfg] : out.quality.kinds) {
        (void)name;
        out.quality.mask_qp_delta_at_255 =
            std::min(out.quality.mask_qp_delta_at_255, cfg.qp_delta);
    }
}

roi::Box box_from_proto(const roi::transcode::Box& b)
{
    roi::Box out;
    out.t_sec = b.t();
    out.kind = b.kind();
    out.x = b.x();
    out.y = b.y();
    out.width = b.width();
    out.height = b.height();
    return out;
}

RoiJobRequest build_request(const roi::transcode::FileJobRequest& req)
{
    RoiJobRequest out;
    out.input_path = require_allowed_path(req.input_path(), "input_path");
    out.output_path = require_allowed_path(req.output_path(), "output_path");
    apply_settings(req.settings(), out.settings);
    out.settings.movflags = req.movflags();

    if (!req.roi_json_path().empty()) {
        RoiJson doc = load_roi_json(
            require_allowed_path(req.roi_json_path(), "roi_json_path"));
        out.settings.quality = doc.quality;
        out.boxes = std::move(doc.boxes);
        return out;
    }

    for (const auto& b : req.boxes()) {
        out.boxes.push_back(box_from_proto(b));
    }
    return out;
}

} // namespace

grpc::Status RoiTranscodeServiceImpl::StartFileJob(
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

grpc::Status RoiTranscodeServiceImpl::GetJob(
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

grpc::Status RoiTranscodeServiceImpl::CancelJob(
    grpc::ServerContext*,
    const roi::transcode::JobId* request,
    roi::transcode::Status* response)
{
    const bool ok = RoiJobManager::instance().cancel(request->job_id());
    response->set_success(ok);
    response->set_message(ok ? "cancelling" : "job unknown or already finished");
    return grpc::Status::OK;
}

grpc::Status RoiTranscodeServiceImpl::ListJobs(
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
class StreamSink : public roi::PacketHandler {
public:
    using Writer = grpc::ServerReaderWriter<roi::transcode::StreamResponse,
        roi::transcode::StreamRequest>;

    explicit StreamSink(Writer* w) : m_writer(w) {}

    void on_open(const roi::StreamInfo& info) override
    {
        roi::transcode::StreamResponse msg;
        auto* opened = msg.mutable_opened();
        opened->set_encoder_used(to_proto(info.encoder_used));
        opened->set_encoder_name(info.encoder_name);
        if (info.extradata && info.extradata_size > 0) {
            opened->set_extradata(info.extradata, info.extradata_size);
        }
        m_writer->Write(msg);
    }

    void on_packet(const roi::EncodedPacket& pkt) override
    {
        roi::transcode::StreamResponse msg;
        auto* out = msg.mutable_packet();
        out->set_pts(pkt.pts);
        out->set_dts(pkt.dts);
        out->set_key(pkt.key);
        out->set_data(pkt.data, pkt.size);
        m_writer->Write(msg);
    }

private:
    Writer* m_writer;
};

roi::InputCodec codec_from_proto(roi::transcode::InputCodec c)
{
    switch (c) {
    case roi::transcode::INPUT_CODEC_H264:
        return roi::InputCodec::H264;
    case roi::transcode::INPUT_CODEC_H265:
        return roi::InputCodec::H265;
    case roi::transcode::INPUT_CODEC_JPEG:
        return roi::InputCodec::Jpeg;
    default:
        return roi::InputCodec::Raw;
    }
}

roi::SessionParams params_from_open(const roi::transcode::StreamOpen& open)
{
    roi::SessionParams sp;
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

grpc::Status RoiTranscodeServiceImpl::EncodeStream(grpc::ServerContext*,
    grpc::ServerReaderWriter<roi::transcode::StreamResponse,
        roi::transcode::StreamRequest>* stream)
{
    roi::transcode::StreamRequest req;
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
        roi::SessionParams sp = params_from_open(open);

        // Either the session muxes into a sandboxed path, or packets come back
        // over the stream. Opened is sent by the handler, on_open.
        std::unique_ptr<StreamSink> streaming;
        if (!open.output_path().empty()) {
            sp.output_path = require_allowed_path(open.output_path(), "output_path");
        } else {
            streaming = std::make_unique<StreamSink>(stream);
        }

        roi::Session session(sp, streaming.get());

        // Raw planes arrive tightly packed, so the plane pointers are derived
        // rather than sent: one layout, described once in `open`.
        const auto pix = static_cast<AVPixelFormat>(open.pix_fmt());
        std::vector<roi::Box> boxes;
        roi::Mask mask;

        while (stream->Read(&req)) {
            if (!req.has_frame()) {
                return {grpc::StatusCode::INVALID_ARGUMENT,
                    "only `frame` may follow `open`"};
            }
            const auto& in = req.frame();

            roi::Regions regions;
            boxes.clear();
            if (in.has_mask()) {
                mask.data =
                    reinterpret_cast<const uint8_t*>(in.mask().data().data());
                mask.width = in.mask().width();
                mask.height = in.mask().height();
                mask.stride = in.mask().width();
                regions.mask = &mask;
            } else if (in.has_boxes()) {
                for (const auto& b : in.boxes().boxes()) {
                    boxes.push_back(box_from_proto(b));
                }
                regions.boxes = boxes.data();
                regions.box_count = boxes.size();
            }

            roi::Frame f;
            f.pts = in.pts();
            const auto* bytes =
                reinterpret_cast<const uint8_t*>(in.data().data());
            if (sp.codec == roi::InputCodec::Raw) {
                uint8_t* planes[4] = {nullptr, nullptr, nullptr, nullptr};
                int strides[4] = {0, 0, 0, 0};
                const int needed = av_image_fill_arrays(planes, strides,
                    bytes, pix, sp.width, sp.height, 1);
                if (needed < 0
                    || static_cast<size_t>(needed) > in.data().size()) {
                    return {grpc::StatusCode::INVALID_ARGUMENT,
                        "raw frame is smaller than the declared format needs"};
                }
                for (int i = 0; i < 4; ++i) {
                    f.plane[i] = planes[i];
                    f.stride[i] = strides[i];
                }
                f.pix_fmt = pix;
                f.width = sp.width;
                f.height = sp.height;
            } else {
                f.encoded = bytes;
                f.encoded_size = in.data().size();
            }
            session.submit(f, regions);
        }

        const roi::Report rep = session.finish();
        roi::transcode::StreamResponse msg;
        auto* done = msg.mutable_done();
        done->set_frames(rep.frames);
        done->set_packets(rep.packets);
        done->set_bytes(rep.bytes);
        done->set_regions_dropped(rep.regions_dropped);
        stream->Write(msg);
        return grpc::Status::OK;
    } catch (const std::exception& ex) {
        return {grpc::StatusCode::INVALID_ARGUMENT, ex.what()};
    }
}
