#include "sve/session.h"

#include "av_raii.h"
#include "frame_input.h"
#include "file_mux_sink.h"
#include "sve/encoder.h"
#include "sve/frame.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <span>

namespace sve {

namespace {

// Regions belong to the frame they were submitted with. A decoder reorders, so
// once frames go in compressed the caller's pointers cannot simply be applied
// to whatever comes out next -- they have to be carried across and matched up.
struct HeldRegions {
    std::vector<uint8_t> mask_bytes;
    Mask mask;
    std::vector<Box> boxes;
    QpDeltaMap qp_map;
    bool has_mask = false;

    Regions view() const
    {
        Regions r;
        if (!qp_map.deltas.empty()) {
            r.qp_map = qp_map;
        } else if (has_mask) {
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
    if (!in.qp_map.deltas.empty()) {
        h.qp_map = in.qp_map;
    } else if (in.mask.consistent()) {
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

// What a stream needs beyond the encoder itself: a decoder for callers who send compressed
// access units, somewhere for the packets to go, and the bookkeeping that reconciles the
// two.
//
// It owns a SmartVideoEncoder and adds nothing to it. The ROI pipeline, the bitrate budget,
// the slew limiter and the region builder used to be assembled here as well as there, and
// the second copy is where the two drifted -- only one of them was feeding its budget from
// its own output. There is one now, and this class is the application layer on top: §3 puts
// decoding and muxing outside the library's scope, and §52 calls this an example of using
// it rather than part of it.
struct Session::Impl {
    SessionParams params;
    std::unique_ptr<FileMuxSink> file;
    PacketHandler* handler = nullptr;
    std::unique_ptr<SmartVideoEncoder> encoder;
    FrameDecoder decoder;
    Report report;
    CodecParameters codec_params;
    bool opened_sink = false;
    bool finished = false;

    // Regions arrive with the access unit that carried them and are applied to the frame
    // the decoder eventually produces for it, which is not the next one out.
    std::map<int64_t, HeldRegions> pending;

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
        // Anything older can no longer be claimed: the decoder emits in presentation
        // order, so those frames were dropped or already handled.
        pending.erase(pending.begin(), std::next(it));
        return held;
    }

    /// Declares the output stream on first use. The encoder knows its codec parameters as
    /// soon as it exists, so this needs no packet to have arrived.
    void open_sink()
    {
        if (opened_sink) {
            return;
        }
        opened_sink = true;
        if (file) {
            file->begin(codec_params);
        }
        if (handler) {
            StreamInfo info;
            info.width = codec_params.width;
            info.height = codec_params.height;
            info.extradata = codec_params.extradata.bytes();
            info.encoder_used = report.encoder_used;
            info.backend_name = report.backend_name;
            handler->on_open(info);
        }
    }

    /// Takes everything the encoder has ready and puts it where the caller asked.
    void drain()
    {
        for (;;) {
            EncodedPacket packet;
            const Status s = encoder->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return;
            }
            if (!s.ok()) {
                roiav::fail(s.message());
            }
            open_sink();
            ++report.packets;
            report.bytes += static_cast<int64_t>(packet.payload.size());
            if (handler) {
                handler->on_packet(packet);
            }
            if (file) {
                file->on_packet(packet);
            }
        }
    }

    void end_sink()
    {
        open_sink();
        if (file) {
            file->end();
        }
        if (handler) {
            handler->on_end();
        }
    }

    FrameMetadata metadata_for(const Regions& regions, int64_t pts) const
    {
        FrameMetadata meta;
        meta.timestamp = MediaTimestamp{pts,
            Rational{params.time_base_num, std::max(params.time_base_den, 1)}};
        meta.regions = regions;
        return meta;
    }

    void encode(AVFrame* frame, const Regions& regions, int64_t pts)
    {
        const Status s = SubmitAvFrame(*encoder, frame, metadata_for(regions, pts));
        if (!s.ok()) {
            roiav::fail(s.message());
        }
        ++report.frames;
        drain();
    }

    void encode_buffer(IFrameBuffer& pixels, const Regions& regions, int64_t pts)
    {
        const Status s = encoder->SubmitFrame(pixels, metadata_for(regions, pts));
        if (!s.ok()) {
            roiav::fail(s.message());
        }
        ++report.frames;
        drain();
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

    EncoderConfig cfg;
    cfg.input.width = params.width;
    cfg.input.height = params.height;
    cfg.input.frame_rate = Rational{params.fps_num, std::max(params.fps_den, 1)};
    cfg.input.time_base =
        Rational{params.time_base_num, std::max(params.time_base_den, 1)};
    cfg.settings = params.settings;
    // SameAsSource, answered from what the caller said it is submitting. Raw frames have no
    // source codec to mirror, so they take the same H.264 fallback an unrecognised one does.
    cfg.settings.codec = params.settings.codec != Codec::SameAsSource
        ? params.settings.codec
        : (params.codec == InputCodec::H265 ? Codec::H265 : Codec::H264);

    // Propagates for the same reason as transcode_file: a Session that cannot open is one
    // exception to the caller, and EncoderError carries the code with it.
    m_impl->encoder = CreateVideoEncoder(cfg);

    const EncoderStatistics stats = m_impl->encoder->GetStatistics();
    m_impl->report.encoder_used = stats.encoder_used;
    m_impl->report.backend_name = stats.backend_name;
    if (const Status s = m_impl->encoder->GetCodecParameters(&m_impl->codec_params);
        !s.ok()) {
        roiav::fail(s.message());
    }

    // After the encoder, and that ordering is the whole point: the decoder is given the
    // device the encoder opened, so its frames are already in the memory the encoder reads
    // and SubmitAvFrame passes them through untouched. Opened before it -- as this was --
    // there is no device to give, and every frame makes the trip down to system memory and
    // back up again. A CPU backend hands over nothing, and then this is the software decode
    // it always was.
    m_impl->decoder.open(params.codec,
        params.extradata.empty() ? nullptr : params.extradata.data(),
        static_cast<int>(params.extradata.size()),
        EncoderHardwareDevice(*m_impl->encoder));
    m_impl->report.decoder_name = m_impl->decoder.decoder_name();
    // Empty until the first frame settles the format, so this is filled again in finish().
    m_impl->report.decoder_hardware = m_impl->decoder.hardware_name();
}

Session::~Session() = default;

Backend Session::encoder_used() const
{
    return m_impl->report.encoder_used;
}

std::string Session::backend_name() const
{
    return m_impl->report.backend_name;
}

std::string Session::decoder_hardware() const
{
    return m_impl->decoder.hardware_name();
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
        if (const Status s = m_impl->encoder->Flush(); !s.ok()) {
            roiav::fail(s.message());
        }
        m_impl->drain();
        // Stop before the sink is ended, matching transcode_file. §8.13 makes Stop the end
        // of an encode, and the §19.4 report on sidecar records that never got a packet
        // lives behind it -- until 2026-09-04 neither of the two paths that actually write
        // a sidecar reached it, so the check existed and never ran where it mattered. The
        // order matters because end_sink tells the caller the stream is complete, and a
        // complaint that it is not has to arrive before that, not after.
        if (const Status s = m_impl->encoder->Stop(); !s.ok()) {
            roiav::fail(s.message());
        }
        m_impl->end_sink();

        // Everything but the frame count comes from the encoder, which is the only thing
        // that knows it. Frames are counted here because a frame submitted and a frame
        // encoded are the same number only when nothing was refused.
        const int64_t frames = m_impl->report.frames;
        const int64_t packets = m_impl->report.packets;
        const int64_t bytes = m_impl->report.bytes;
        m_impl->report = m_impl->encoder->ToReport();
        m_impl->report.frames = frames;
        m_impl->report.packets = packets;
        m_impl->report.bytes = bytes;
        // The encoder's report knows nothing about the decoder in front of it, and this
        // assignment replaces the whole struct -- so the two decoder fields are put back
        // here, and the hardware one is read now rather than at open: the format was
        // negotiated on the first packet, which had not arrived then.
        m_impl->report.decoder_name = m_impl->decoder.decoder_name();
        m_impl->report.decoder_hardware = m_impl->decoder.hardware_name();
    }
    return m_impl->report;
}

} // namespace sve
