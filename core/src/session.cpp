#include "roi/session.h"

#include "av_raii.h"
#include "frame_input.h"
#include "packet_sink.h"
#include "roi_encoder.h"
#include "roi_map_builder.h"

#include <algorithm>
#include <map>

namespace roi {

namespace {

// Regions belong to the frame they were submitted with. A decoder reorders, so
// once frames go in compressed the caller's pointers cannot simply be applied
// to whatever comes out next -- they have to be carried across and matched up.
struct HeldRegions {
    std::vector<uint8_t> mask_bytes;
    RoiMask mask;
    std::vector<RoiSample> boxes;
    bool has_mask = false;

    Regions view() const
    {
        Regions r;
        if (has_mask) {
            r.mask = &mask;
        } else if (!boxes.empty()) {
            r.boxes = boxes.data();
            r.box_count = boxes.size();
        }
        return r;
    }
};

HeldRegions hold(const Regions& in)
{
    HeldRegions h;
    if (in.mask && in.mask->data && in.mask->width > 0 && in.mask->height > 0) {
        const int stride = in.mask->stride > 0 ? in.mask->stride : in.mask->width;
        h.mask_bytes.resize(
            static_cast<size_t>(in.mask->width) * in.mask->height);
        for (int y = 0; y < in.mask->height; ++y) {
            std::copy_n(in.mask->data + static_cast<size_t>(y) * stride,
                in.mask->width,
                h.mask_bytes.begin() + static_cast<size_t>(y) * in.mask->width);
        }
        h.mask.data = h.mask_bytes.data();
        h.mask.width = in.mask->width;
        h.mask.height = in.mask->height;
        h.mask.stride = in.mask->width;
        h.has_mask = true;
    } else if (in.boxes && in.box_count > 0) {
        h.boxes.assign(in.boxes, in.boxes + in.box_count);
    }
    return h;
}

} // namespace

// Sits between the encoder and the caller's sink: every packet is counted on
// its way through, so finish() can report without the caller tallying.
struct Session::Impl : PacketSink {
    SessionParams params;
    PacketSink* out = nullptr;
    RoiEncoder encoder;
    FrameDecoder decoder;
    RoiMapBuilder boxes;
    roiav::Frame wrapper;
    Report report;
    bool finished = false;
    // Submitted-but-not-yet-decoded regions, keyed by the pts they arrived with.
    std::map<int64_t, HeldRegions> pending;

    // Matches a decoded frame to the regions submitted with it. Falls back to
    // the oldest entry when the stream carries no usable timestamps, which is
    // the right guess for a decoder that is not reordering.
    HeldRegions take_pending(int64_t pts, bool have_pts)
    {
        if (pending.empty()) {
            return HeldRegions{};
        }
        auto it = have_pts ? pending.find(pts) : pending.end();
        if (it == pending.end()) {
            if (have_pts) {
                return HeldRegions{};
            }
            it = pending.begin();
        }
        HeldRegions held = std::move(it->second);
        // Anything older can no longer be claimed: the decoder emits in
        // presentation order, so those frames were dropped or already handled.
        pending.erase(pending.begin(), std::next(it));
        return held;
    }

    void begin(AVCodecContext* enc) override { out->begin(enc); }
    void on_packet(AVPacket* pkt, AVRational tb) override
    {
        ++report.packets;
        report.bytes += pkt->size;
        out->on_packet(pkt, tb);
    }
    void end() override { out->end(); }

    std::vector<RoiRect> rects_for(const Regions& regions)
    {
        const int w = params.width;
        const int h = params.height;

        if (regions.mask && regions.mask->data) {
            const QpDeltaMap grid =
                qp_map_from_mask(*regions.mask, params.mask_mapping, w, h);
            int dropped = 0;
            auto rects = rects_from_qp_map(grid, params.max_regions, &dropped);
            report.regions_dropped += dropped;
            return rects;
        }

        std::vector<RoiSample> samples;
        if (regions.boxes && regions.box_count > 0) {
            samples.assign(regions.boxes, regions.boxes + regions.box_count);
        }
        const auto dets =
            boxes.detections_from(samples, params.box_quality.kinds, w, h);
        auto rects =
            boxes.build(w, h, dets, params.box_quality.background_qp_delta);
        if (params.max_regions > 0
            && static_cast<int>(rects.size()) > params.max_regions) {
            report.regions_dropped +=
                static_cast<int>(rects.size()) - params.max_regions;
            rects.resize(static_cast<size_t>(params.max_regions));
        }
        return rects;
    }

    void encode(AVFrame* frame, const Regions& regions, int64_t pts)
    {
        encoder.encode(frame, rects_for(regions), pts);
        ++report.frames;
    }
};

Session::Session(const SessionParams& params, PacketSink& sink)
    : m_impl(std::make_unique<Impl>())
{
    if (params.width <= 0 || params.height <= 0) {
        roiav::fail("session needs a frame size");
    }

    m_impl->params = params;
    m_impl->out = &sink;

    m_impl->decoder.open(params.codec,
        params.extradata.empty() ? nullptr : params.extradata.data(),
        static_cast<int>(params.extradata.size()));

    EncoderParams ep;
    ep.width = params.width;
    ep.height = params.height;
    ep.fps = AVRational{params.fps_num, std::max(params.fps_den, 1)};
    ep.time_base =
        AVRational{params.time_base_num, std::max(params.time_base_den, 1)};
    ep.kind = params.encoder;
    ep.crf = params.crf;
    ep.gop = params.gop;
    ep.preset = params.preset;

    m_impl->encoder.open(ep, *m_impl);
    m_impl->report.encoder_used = m_impl->encoder.kind();
    m_impl->report.encoder_name = m_impl->encoder.label();
}

Session::~Session() = default;

EncoderKind Session::encoder_used() const
{
    return m_impl->report.encoder_used;
}

std::string Session::encoder_name() const
{
    return m_impl->report.encoder_name;
}

void Session::submit(const Frame& frame, const Regions& regions)
{
    if (m_impl->params.codec == InputCodec::Raw) {
        AVFrame* f = m_impl->wrapper.p;
        av_frame_unref(f);
        f->format = frame.pix_fmt;
        f->width = frame.width > 0 ? frame.width : m_impl->params.width;
        f->height = frame.height > 0 ? frame.height : m_impl->params.height;
        for (int i = 0; i < 4; ++i) {
            f->data[i] = const_cast<uint8_t*>(frame.plane[i]);
            f->linesize[i] = frame.stride[i];
        }
        m_impl->encode(f, regions, frame.pts);
        return;
    }

    m_impl->pending.emplace(frame.pts, hold(regions));
    m_impl->decoder.push(frame.encoded, frame.encoded_size, frame.pts,
        [&](AVFrame* decoded) {
            const bool have_pts =
                decoded->best_effort_timestamp != AV_NOPTS_VALUE;
            const int64_t pts =
                have_pts ? decoded->best_effort_timestamp : frame.pts;
            const HeldRegions held = m_impl->take_pending(pts, have_pts);
            m_impl->encode(decoded, held.view(), pts);
        });
}

Report Session::finish()
{
    if (!m_impl->finished) {
        m_impl->finished = true;
        m_impl->decoder.flush([&](AVFrame* decoded) {
            const bool have_pts =
                decoded->best_effort_timestamp != AV_NOPTS_VALUE;
            const int64_t pts = have_pts ? decoded->best_effort_timestamp : 0;
            const HeldRegions held = m_impl->take_pending(pts, have_pts);
            m_impl->encode(decoded, held.view(), pts);
        });
        m_impl->encoder.flush();
        m_impl->out->end();
    }
    return m_impl->report;
}

} // namespace roi
