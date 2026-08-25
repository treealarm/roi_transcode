#include "smartvideo/session.h"

#include "av_raii.h"
#include "frame_input.h"
#include "packet_sink.h"
#include "roi_encoder.h"
#include "object_normalizer.h"

#include <algorithm>
#include <span>
#include <map>

namespace smartvideo {

namespace {

// Regions belong to the frame they were submitted with. A decoder reorders, so
// once frames go in compressed the caller's pointers cannot simply be applied
// to whatever comes out next -- they have to be carried across and matched up.
struct HeldRegions {
    std::vector<uint8_t> mask_bytes;
    Mask mask;
    std::vector<Box> boxes;
    bool has_mask = false;

    Regions view() const
    {
        Regions r;
        if (has_mask) {
            r.mask = mask;
        } else if (!boxes.empty()) {
            r.boxes = boxes;
        }
        return r;
    }
};

HeldRegions hold(const Regions& in)
{
    HeldRegions h;
    if (in.mask.consistent()) {
        const int32_t stride = in.mask.stride > 0 ? in.mask.stride : in.mask.width;
        h.mask_bytes.resize(
            static_cast<size_t>(in.mask.width) * in.mask.height);
        for (int32_t y = 0; y < in.mask.height; ++y) {
            std::copy_n(in.mask.data.data() + static_cast<size_t>(y) * stride,
                in.mask.width,
                h.mask_bytes.begin() + static_cast<size_t>(y) * in.mask.width);
        }
        h.mask.data = h.mask_bytes;
        h.mask.width = in.mask.width;
        h.mask.height = in.mask.height;
        h.mask.stride = in.mask.width;
        h.has_mask = true;
    } else if (!in.boxes.empty()) {
        h.boxes.assign(in.boxes.begin(), in.boxes.end());
    }
    return h;
}

} // namespace

// Sits between the encoder and wherever packets are going: a file it owns, or
// the caller's handler. Counts them on the way through so finish() can report
// without the caller tallying.
struct Session::Impl : PacketSink {
    SessionParams params;
    std::unique_ptr<FileMuxSink> file;
    PacketHandler* handler = nullptr;
    RoiEncoder encoder;
    FrameDecoder decoder;
    RoiMapBuilder builder;
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

    void begin(AVCodecContext* enc) override
    {
        if (file) {
            file->begin(enc);
        }
        if (handler) {
            StreamInfo info;
            info.width = enc->width;
            info.height = enc->height;
            if (enc->extradata && enc->extradata_size > 0) {
                info.extradata = std::span<const uint8_t>(enc->extradata,
                    static_cast<std::size_t>(enc->extradata_size));
            }
            info.encoder_used = report.encoder_used;
            info.encoder_name = report.encoder_name;
            handler->on_open(info);
        }
    }

    void on_packet(AVPacket* pkt, AVRational tb) override
    {
        ++report.packets;
        report.bytes += pkt->size;
        if (handler) {
            EncodedPacket out;
            out.data = std::span<const uint8_t>(
                pkt->data, static_cast<std::size_t>(pkt->size));
            out.pts = pkt->pts;
            out.dts = pkt->dts;
            out.duration = pkt->duration;
            // The units the timestamps above are in. Filling this is not optional: the
            // field defaults to 1/1, and a handler that believed it while the session runs
            // in milliseconds -- which is the default -- would be out by a factor of a
            // thousand, consistently, in a way that looks like working code.
            out.time_base = Rational{tb.num, tb.den};
            out.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            handler->on_packet(out);
        }
        if (file) {
            file->on_packet(pkt, tb);
        }
    }

    void end() override
    {
        if (file) {
            file->end();
        }
        if (handler) {
            handler->on_end();
        }
    }

    /// How many regions may actually be sent this frame.
    ///
    /// Two numbers meet here: what the caller asked for, and what the backend that opened
    /// will accept (TZ v2.0 §6.4). The smaller wins. Before this the setting was obeyed
    /// alone, which meant a request for 200 regions was silently truncated by the driver
    /// instead of by us -- and the count in Report.regions_dropped was wrong, because we
    /// did not know the loss had happened.
    int region_budget() const
    {
        const int asked = params.settings.quality.max_regions;
        const uint32_t limit = encoder.capabilities().max_roi_regions;
        if (limit == 0) {
            return asked;  // the backend names no limit; the caller's word stands
        }
        const int cap = static_cast<int>(limit);
        return asked > 0 ? std::min(asked, cap) : cap;
    }

    std::vector<Rect> rects_for(const Regions& regions)
    {
        const int32_t w = params.width;
        const int32_t h = params.height;

        if (regions.mask.consistent()) {
            // The mask has no kind names: it interpolates between the
            // background and the strongest strength the settings name.
            MaskMapping mapping;
            mapping.qp_delta_at_0 = params.settings.quality.background_qp_delta;
            mapping.qp_delta_at_255 = params.settings.quality.mask_qp_delta_at_255;
            mapping.levels = params.settings.quality.mask_levels;
            const QpDeltaMap grid = qp_map_from_mask(regions.mask, mapping, w, h);
            int32_t dropped = 0;
            auto rects = rects_from_qp_map(grid, region_budget(), &dropped);
            report.regions_dropped += dropped;
            return rects;
        }

        const std::vector<Box> box_list(regions.boxes.begin(), regions.boxes.end());
        const auto dets =
            builder.detections_from(box_list, params.settings.quality, w, h);
        auto rects =
            builder.build(w, h, dets, params.settings.quality);
        const int budget = region_budget();
        if (budget > 0 && static_cast<int>(rects.size()) > budget) {
            report.regions_dropped += static_cast<int>(rects.size()) - budget;
            rects.resize(static_cast<size_t>(budget));
        }
        return rects;
    }

    // Pixels the caller owns, handed straight to the encoder. No decode, no wrapping:
    // this is the path a factory frame takes, and the one where zero copies is possible.
    void encode_buffer(IFrameBuffer& pixels, const Regions& regions, int64_t pts)
    {
        encoder.encode_buffer(pixels, rects_for(regions), pts);
        ++report.frames;
    }

    void encode(AVFrame* frame, const Regions& regions, int64_t pts)
    {
        encoder.encode(frame, rects_for(regions), pts);
        ++report.frames;
    }
};

Session::Session(const SessionParams& params, PacketHandler* handler)
    : m_impl(std::make_unique<Impl>())
{
    if (params.width <= 0 || params.height <= 0) {
        roiav::fail("session needs a frame size");
    }
    if (params.output_path.empty() && !handler) {
        roiav::fail("session needs either an output_path or a packet handler");
    }

    m_impl->params = params;
    m_impl->handler = handler;
    if (!params.output_path.empty()) {
        m_impl->file = std::make_unique<FileMuxSink>(
            params.output_path, params.settings.movflags);
    }

    m_impl->decoder.open(params.codec,
        params.extradata.empty() ? nullptr : params.extradata.data(),
        static_cast<int>(params.extradata.size()));

    EncoderConfig ep;
    ep.width = params.width;
    ep.height = params.height;
    ep.fps = Rational{params.fps_num, std::max(params.fps_den, 1)};
    ep.time_base =
        Rational{params.time_base_num, std::max(params.time_base_den, 1)};
    ep.kind = params.settings.encoder;
    ep.policy = params.settings.policy;
    ep.temporal = params.settings.temporal;
    ep.rate_control = params.settings.rate_control;
    // SameAsSource, answered from what the caller said it is submitting. Raw frames have no
    // source codec to mirror, so they take the same H.264 fallback an unrecognised one does.
    ep.codec = params.settings.codec != Codec::SameAsSource ? params.settings.codec
        : (params.codec == InputCodec::H265 ? Codec::H265 : Codec::H264);
    ep.crf = params.settings.crf;
    ep.gop = params.settings.gop;
    ep.preset = params.settings.preset;

    // The report is filled before begin() runs, because StreamInfo carries the
    // backend and the sink is what reports it.
    m_impl->encoder.open_deferred(ep);
    m_impl->report.encoder_used = m_impl->encoder.kind();
    m_impl->report.encoder_name = m_impl->encoder.label();
    m_impl->encoder.attach(*m_impl);
}

Session::~Session() = default;

Encoder Session::encoder_used() const
{
    return m_impl->report.encoder_used;
}

std::string Session::encoder_name() const
{
    return m_impl->report.encoder_name;
}

void Session::submit(IFrameBuffer& pixels, const Regions& regions, int64_t pts)
{
    if (m_impl->params.codec != InputCodec::Raw) {
        roiav::fail("this session was opened with a codec; submit an access unit");
    }
    m_impl->encode_buffer(pixels, regions, pts);
}

void Session::submit(
    std::span<const uint8_t> access_unit, const Regions& regions, int64_t pts)
{
    if (m_impl->params.codec == InputCodec::Raw) {
        roiav::fail("this session was opened for raw frames; submit pixels");
    }
    if (access_unit.empty()) {
        return;
    }
    m_impl->pending.emplace(pts, hold(regions));
    m_impl->decoder.push(access_unit.data(), access_unit.size(), pts,
        [&](AVFrame* decoded) {
            const bool have_pts =
                decoded->best_effort_timestamp != AV_NOPTS_VALUE;
            const int64_t decoded_pts =
                have_pts ? decoded->best_effort_timestamp : pts;
            const HeldRegions held = m_impl->take_pending(decoded_pts, have_pts);
            m_impl->encode(decoded, held.view(), decoded_pts);
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
        m_impl->end();
        m_impl->report.frames_converted = m_impl->encoder.frames_converted();
        m_impl->report.frames_uploaded = m_impl->encoder.frames_uploaded();
    }
    return m_impl->report;
}

} // namespace smartvideo
