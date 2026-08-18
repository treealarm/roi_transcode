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

EncoderKind from_proto(roi::transcode::Encoder e)
{
    switch (e) {
    case roi::transcode::ENCODER_QSV:
        return EncoderKind::Qsv;
    case roi::transcode::ENCODER_VAAPI:
        return EncoderKind::Vaapi;
    case roi::transcode::ENCODER_NVENC:
        return EncoderKind::Nvenc;
    case roi::transcode::ENCODER_X265:
        return EncoderKind::X265;
    default:
        return EncoderKind::Auto;
    }
}

roi::transcode::Encoder to_proto(EncoderKind k)
{
    switch (k) {
    case EncoderKind::Qsv:
        return roi::transcode::ENCODER_QSV;
    case EncoderKind::Vaapi:
        return roi::transcode::ENCODER_VAAPI;
    case EncoderKind::Nvenc:
        return roi::transcode::ENCODER_NVENC;
    case EncoderKind::X265:
        return roi::transcode::ENCODER_X265;
    default:
        return roi::transcode::ENCODER_AUTO;
    }
}

// Encoder knobs are shared by both entry points, so unpacking them is too.
void apply_settings(const roi::transcode::Settings& in, EncoderKind& kind,
    int& crf, int& gop, std::string& preset)
{
    kind = from_proto(in.encoder());
    if (in.crf() > 0) {
        crf = in.crf();
    }
    if (in.gop() > 0) {
        gop = in.gop();
    }
    if (!in.preset().empty()) {
        preset = in.preset();
    }
}

void apply_kinds(const roi::transcode::RoiQuality& q,
    std::unordered_map<std::string, KindConfig>& out)
{
    for (const auto& k : q.kinds()) {
        KindConfig cfg;
        if (k.has_qp_delta()) {
            cfg.qp_delta = k.qp_delta();
        }
        if (k.has_pad() && k.pad() > 0) {
            cfg.pad_norm = k.pad();
        }
        out.emplace(k.kind(), cfg);
    }
}

RoiJobRequest build_request(const roi::transcode::FileJobRequest& req)
{
    RoiJobRequest out;
    out.options.input_path = require_allowed_path(req.input_path(), "input_path");
    out.options.output_path = require_allowed_path(req.output_path(), "output_path");
    out.options.movflags = req.movflags();
    apply_settings(req.settings(), out.options.encoder, out.options.crf,
        out.options.gop, out.options.preset);

    if (!req.roi_json_path().empty()) {
        out.options.roi = load_roi_json(
            require_allowed_path(req.roi_json_path(), "roi_json_path"));
        return out;
    }

    RoiDocument doc;
    const auto& q = req.settings().roi();
    if (q.has_background_qp_delta()) {
        doc.background_qp_delta = q.background_qp_delta();
    }
    if (q.has_hold_sec() && q.hold_sec() > 0) {
        doc.hold_sec = q.hold_sec();
    }
    apply_kinds(q, doc.kinds);
    for (const auto& b : req.boxes()) {
        RoiSample s;
        s.t = b.t();
        s.kind = b.kind();
        s.bbox_x = b.x();
        s.bbox_y = b.y();
        s.bbox_w = b.width();
        s.bbox_h = b.height();
        doc.samples.push_back(s);
    }
    out.options.roi = std::move(doc);
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

// Ships packets back over the stream. The alternative sink writes a file, in
// which case nothing is streamed and only the summary comes back.
class StreamSink : public PacketSink {
public:
    using Writer = grpc::ServerReaderWriter<roi::transcode::StreamResponse,
        roi::transcode::StreamRequest>;

    explicit StreamSink(Writer* w) : m_writer(w) {}

    // Only records: which backend opened is known to the Session, not to the
    // sink, and Opened should carry both that and the extradata in one message.
    void begin(AVCodecContext* enc) override
    {
        if (enc->extradata && enc->extradata_size > 0) {
            m_extradata.assign(enc->extradata, enc->extradata + enc->extradata_size);
        }
    }

    void send_opened(roi::transcode::Encoder kind, const std::string& name)
    {
        roi::transcode::StreamResponse msg;
        auto* opened = msg.mutable_opened();
        opened->set_encoder_used(kind);
        opened->set_encoder_name(name);
        if (!m_extradata.empty()) {
            opened->set_extradata(m_extradata.data(), m_extradata.size());
        }
        m_writer->Write(msg);
    }

    void on_packet(AVPacket* pkt, AVRational) override
    {
        roi::transcode::StreamResponse msg;
        auto* out = msg.mutable_packet();
        out->set_pts(pkt->pts);
        out->set_dts(pkt->dts);
        out->set_key((pkt->flags & AV_PKT_FLAG_KEY) != 0);
        out->set_data(pkt->data, static_cast<size_t>(pkt->size));
        m_writer->Write(msg);
    }

    void end() override {}

private:
    Writer* m_writer;
    std::vector<uint8_t> m_extradata;
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
    apply_settings(open.settings(), sp.encoder, sp.crf, sp.gop, sp.preset);

    const auto& q = open.settings().roi();
    if (q.has_background_qp_delta()) {
        sp.box_quality.background_qp_delta = q.background_qp_delta();
        sp.mask_mapping.qp_delta_at_0 = q.background_qp_delta();
    }
    if (q.max_regions() > 0) {
        sp.max_regions = q.max_regions();
    }
    apply_kinds(q, sp.box_quality.kinds);
    // A mask carries one strength, so it takes the strongest kind on offer.
    for (const auto& [name, cfg] : sp.box_quality.kinds) {
        (void)name;
        sp.mask_mapping.qp_delta_at_255 =
            std::min(sp.mask_mapping.qp_delta_at_255, cfg.qp_delta);
    }
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
        const roi::SessionParams sp = params_from_open(open);

        std::unique_ptr<PacketSink> sink;
        StreamSink* streaming = nullptr;
        if (!open.output_path().empty()) {
            sink = std::make_unique<FileMuxSink>(
                require_allowed_path(open.output_path(), "output_path"),
                open.movflags());
        } else {
            auto s = std::make_unique<StreamSink>(stream);
            streaming = s.get();
            sink = std::move(s);
        }

        roi::Session session(sp, *sink);
        if (streaming) {
            streaming->send_opened(
                to_proto(session.encoder_used()), session.encoder_name());
        }

        // Raw planes arrive tightly packed, so the plane pointers are derived
        // rather than sent: one layout, described once in `open`.
        const auto pix = static_cast<AVPixelFormat>(open.pix_fmt());
        std::vector<RoiSample> boxes;
        RoiMask mask;

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
                    RoiSample s;
                    s.t = b.t();
                    s.kind = b.kind();
                    s.bbox_x = b.x();
                    s.bbox_y = b.y();
                    s.bbox_w = b.width();
                    s.bbox_h = b.height();
                    boxes.push_back(s);
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
