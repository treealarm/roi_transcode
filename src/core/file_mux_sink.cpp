#include "packet_sink.h"

#include "codec_id.h"

#include <cstring>

#include <utility>

extern "C" {
#include <libavutil/dict.h>
}
namespace sve {


FileMuxSink::FileMuxSink(std::string path, std::string movflags)
    : m_path(std::move(path)), m_movflags(std::move(movflags))
{
    roiav::check(
        avformat_alloc_output_context2(&m_out.p, nullptr, nullptr, m_path.c_str()),
        "alloc output");
}

void FileMuxSink::declare_audio(const AVCodecParameters* par, AVRational time_base)
{
    m_audio_par = par;
    m_audio_tb = time_base;
}

void FileMuxSink::begin(const CodecParameters& params)
{
    if (m_started) {
        return;
    }
    open_video_stream();
    m_video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    m_video->codecpar->codec_id = av_id_from_codec(params.codec);
    m_video->codecpar->width = params.width;
    m_video->codecpar->height = params.height;
    if (!params.extradata.empty()) {
        m_video->codecpar->extradata = static_cast<uint8_t*>(
            av_mallocz(params.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (m_video->codecpar->extradata == nullptr) {
            roiav::fail("allocate extradata");
        }
        std::memcpy(m_video->codecpar->extradata, params.extradata.data(),
            params.extradata.size());
        m_video->codecpar->extradata_size = static_cast<int>(params.extradata.size());
    }
    m_video->time_base = AVRational{params.time_base.num, params.time_base.den};
    finish_begin();
}

void FileMuxSink::open_video_stream()
{
    m_video = avformat_new_stream(m_out.p, nullptr);
    if (!m_video) {
        roiav::fail("new video stream");
    }
}

void FileMuxSink::finish_begin()
{

    if (m_audio_par) {
        m_audio = avformat_new_stream(m_out.p, nullptr);
        if (!m_audio) {
            roiav::fail("new audio stream");
        }
        roiav::check(avcodec_parameters_copy(m_audio->codecpar, m_audio_par),
            "copy audio params");
        m_audio->time_base = m_audio_tb;
    }

    if (!(m_out.p->oformat->flags & AVFMT_NOFILE)) {
        roiav::check(avio_open(&m_out.p->pb, m_path.c_str(), AVIO_FLAG_WRITE),
            "avio_open");
    }

    AVDictionary* opts = nullptr;
    if (!m_movflags.empty()) {
        av_dict_set(&opts, "movflags", m_movflags.c_str(), 0);
    }
    const int r = avformat_write_header(m_out.p, &opts);
    av_dict_free(&opts);
    roiav::check(r, "write header");
    m_started = true;
}

void FileMuxSink::write_muxed(AVPacket* pkt, AVRational enc_time_base)
{
    av_packet_rescale_ts(pkt, enc_time_base, m_video->time_base);
    pkt->stream_index = m_video->index;

    // Nudge a repeated decode timestamp forward by one tick.
    //
    // The mp4 muxer refuses a DTS that does not advance, and returns EINVAL -- which
    // arrives here as "write video: Invalid argument" partway through a file, with nothing
    // to say which frame caused it. Real archive clips do contain repeated timestamps: of
    // 543 clips this deployment has tried to re-encode, 19 exhausted their attempts and
    // landed in roi_state='failed', and the ones checked all carry duplicate DTS in the
    // source while a clip that succeeded carries none. ffmpeg reports the same thing
    // decoding them to /dev/null, so it is the recording that repeats, not this encoder.
    //
    // A tick rather than dropping the frame: the picture is real and the operator is
    // watching it, so losing one to a timestamp the camera got wrong is the wrong trade.
    // One tick is below anything a player can show and keeps the count intact.
    if (m_last_dts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE
        && pkt->dts <= m_last_dts) {
        const int64_t shift = m_last_dts + 1 - pkt->dts;
        pkt->dts += shift;
        // Presentation cannot precede decode; move it with the DTS rather than clamping,
        // so a frame's own display order relative to its neighbours is unchanged.
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
            pkt->pts += shift;
        }
    }
    m_last_dts = pkt->dts;

    roiav::check(av_interleaved_write_frame(m_out.p, pkt), "write video");
}

void FileMuxSink::on_packet(const EncodedPacket& packet)
{
    roiav::Packet pkt;
    if (av_new_packet(pkt.p, static_cast<int>(packet.payload.size())) < 0) {
        roiav::fail("allocate packet for muxing");
    }
    std::memcpy(pkt.p->data, packet.payload.data(), packet.payload.size());
    pkt.p->pts = packet.pts;
    pkt.p->dts = packet.dts;
    pkt.p->duration = packet.duration;
    if (packet.key_frame) {
        pkt.p->flags |= AV_PKT_FLAG_KEY;
    }
    write_muxed(pkt.p, AVRational{packet.time_base.num, packet.time_base.den});
}

void FileMuxSink::write_audio(AVPacket* pkt, AVRational src_time_base)
{
    if (!m_audio) {
        return;
    }
    av_packet_rescale_ts(pkt, src_time_base, m_audio->time_base);
    pkt->stream_index = m_audio->index;
    roiav::check(av_interleaved_write_frame(m_out.p, pkt), "write audio");
}

void FileMuxSink::end()
{
    if (!m_started || m_finished) {
        return;
    }
    m_finished = true;
    roiav::check(av_write_trailer(m_out.p), "write trailer");
}

} // namespace sve
