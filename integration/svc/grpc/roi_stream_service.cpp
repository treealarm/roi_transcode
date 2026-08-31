#include "grpc/roi_transcode_service.h"

#include "grpc/path_guard.h"
#include "grpc/proto_convert.h"
#include "sve/session.h"

#include <cstring>
#include <memory>
#include <span>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

using namespace roisvc;

namespace {

// Ships packets back over the stream. The alternative is output_path, in which
// case the session muxes and only the summary comes back.
class StreamSink : public sve::PacketHandler {
public:
    using Writer = grpc::ServerReaderWriter<roi::transcode::EncodeStreamResponse,
        roi::transcode::EncodeStreamRequest>;

    explicit StreamSink(Writer* w) : m_writer(w) {}

    void on_open(const sve::StreamInfo& info) override
    {
        roi::transcode::EncodeStreamResponse msg;
        auto* opened = msg.mutable_opened();
        opened->set_encoder_used(to_proto(info.encoder_used));
        opened->set_encoder_name(info.backend_name);
        if (!info.extradata.empty()) {
            opened->set_extradata(info.extradata.data(), info.extradata.size());
        }
        m_writer->Write(msg);
    }

    void on_packet(const sve::EncodedPacket& pkt) override
    {
        roi::transcode::EncodeStreamResponse msg;
        auto* out = msg.mutable_packet();
        out->set_pts(pkt.pts);
        out->set_dts(pkt.dts);
        out->set_key(pkt.key_frame);
        out->set_data(pkt.payload.data(), pkt.payload.size());
        m_writer->Write(msg);
    }

private:
    Writer* m_writer;
};

} // namespace

grpc::Status RoiStreamTranscodeServiceImpl::EncodeStream(grpc::ServerContext*,
    grpc::ServerReaderWriter<roi::transcode::EncodeStreamResponse,
        roi::transcode::EncodeStreamRequest>* stream)
{
    roi::transcode::EncodeStreamRequest req;
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
        sve::SessionParams sp = params_from_open(open);

        // Either the session muxes into a sandboxed path, or packets come back
        // over the stream. Opened is sent by the handler, on_open.
        std::unique_ptr<StreamSink> streaming;
        if (!open.output_path().empty()) {
            sp.output_path = require_allowed_path(open.output_path(), "output_path");
        } else {
            streaming = std::make_unique<StreamSink>(stream);
        }

        sve::Session session(sp, streaming.get());

        // Raw planes arrive tightly packed, so the plane pointers are derived
        // rather than sent: one layout, described once in `open`.
        const auto pix = static_cast<AVPixelFormat>(open.pix_fmt());
        // The wire still carries an AVPixelFormat number -- that is ta_vms' own contract
        // with this service. The library has its own enum, so the translation happens here,
        // at the edge, rather than leaking FFmpeg's numbering into the library's surface.
        const sve::PixelFormat frame_format =
            sp.codec == sve::InputCodec::Raw
            ? to_library_format(pix)
            : sve::PixelFormat::Unknown;
        if (sp.codec == sve::InputCodec::Raw
            && frame_format == sve::PixelFormat::Unknown) {
            return {grpc::StatusCode::INVALID_ARGUMENT,
                "pix_fmt names a format this encoder does not take"};
        }
        std::vector<sve::Box> boxes;
        sve::Mask mask;

        while (stream->Read(&req)) {
            if (!req.has_frame()) {
                return {grpc::StatusCode::INVALID_ARGUMENT,
                    "only `frame` may follow `open`"};
            }
            const auto& in = req.frame();

            sve::Regions regions;
            boxes.clear();
            if (in.has_qp_map()) {
                const auto& m = in.qp_map();
                sve::QpDeltaMap grid;
                grid.block = m.block();
                grid.cols = m.cols();
                grid.rows = m.rows();
                grid.width = m.width() > 0 ? m.width() : sp.width;
                grid.height = m.height() > 0 ? m.height() : sp.height;
                const auto& raw = m.deltas();
                grid.deltas.resize(raw.size());
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    grid.deltas[i] = static_cast<int8_t>(raw[i]);
                }
                const std::size_t expect =
                    static_cast<std::size_t>(std::max(grid.cols, 0))
                    * static_cast<std::size_t>(std::max(grid.rows, 0));
                if (grid.block <= 0 || grid.cols <= 0 || grid.rows <= 0
                    || grid.deltas.size() != expect) {
                    return {grpc::StatusCode::INVALID_ARGUMENT,
                        "qp_map deltas length must equal cols*rows with block>0"};
                }
                regions.qp_map = std::move(grid);
            } else if (in.has_mask()) {
                mask.data = std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(in.mask().data().data()),
                    in.mask().data().size());
                mask.width = in.mask().width();
                mask.height = in.mask().height();
                mask.stride = in.mask().width();
                regions.mask = mask;
            } else if (in.has_boxes()) {
                for (const auto& b : in.boxes().boxes()) {
                    boxes.push_back(box_from_proto(b));
                }
                regions.boxes = boxes;
            }

            const auto* bytes =
                reinterpret_cast<const uint8_t*>(in.data().data());
            if (sp.codec == sve::InputCodec::Raw) {
                uint8_t* planes[4] = {nullptr, nullptr, nullptr, nullptr};
                int strides[4] = {0, 0, 0, 0};
                const int needed = av_image_fill_arrays(planes, strides,
                    bytes, pix, sp.width, sp.height, 1);
                if (needed < 0
                    || static_cast<size_t>(needed) > in.data().size()) {
                    return {grpc::StatusCode::INVALID_ARGUMENT,
                        "raw frame is smaller than the declared format needs"};
                }
                // Each plane's length is the distance to the next one, and for the last
                // the remainder of what the frame occupies. Derived here rather than
                // guessed inside the library, which is the point of PlaneBuffer asking.
                sve::PlaneBuffer::Plane described[4];
                for (int i = 0; i < 4; ++i) {
                    if (!planes[i]) {
                        continue;
                    }
                    const uint8_t* next = nullptr;
                    for (int j = i + 1; j < 4 && !next; ++j) {
                        next = planes[j];
                    }
                    const std::size_t len = next
                        ? static_cast<std::size_t>(next - planes[i])
                        : static_cast<std::size_t>(needed)
                            - static_cast<std::size_t>(planes[i] - bytes);
                    described[i] = {std::span<uint8_t>(planes[i], len), strides[i]};
                }
                sve::PlaneBuffer pixels(sp.width, sp.height, frame_format,
                    std::span<const sve::PlaneBuffer::Plane>(described, 4));
                session.submit(pixels, regions, in.pts());
            } else {
                session.submit(
                    std::span<const uint8_t>(bytes, in.data().size()), regions,
                    in.pts());
            }
        }

        const sve::Report rep = session.finish();
        roi::transcode::EncodeStreamResponse msg;
        auto* done = msg.mutable_done();
        done->set_frames(rep.frames);
        done->set_packets(rep.packets);
        done->set_bytes(rep.bytes);
        done->set_regions_dropped(rep.regions_dropped);
        done->set_cpu_h265_target_guard(rep.cpu_h265_target_guard);
        fill_lag(rep.roi_lag_measured, rep.roi_lag_samples, rep.roi_lag_min_iou,
            rep.roi_lag_p95_frames, rep.roi_lag_p99_frames, rep.roi_lag_p95_ms,
            rep.roi_lag_p99_ms, rep.roi_lag_acceptance, done->mutable_roi_lag());
        stream->Write(msg);
        return grpc::Status::OK;
    } catch (const std::exception& ex) {
        return {grpc::StatusCode::INVALID_ARGUMENT, ex.what()};
    }
}
