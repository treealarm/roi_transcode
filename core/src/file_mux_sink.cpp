#include "packet_sink.h"

#include <utility>

extern "C" {
#include <libavutil/dict.h>
}

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

void FileMuxSink::begin(AVCodecContext* enc)
{
    if (m_started) {
        return;
    }

    m_video = avformat_new_stream(m_out.p, nullptr);
    if (!m_video) {
        roiav::fail("new video stream");
    }
    roiav::check(avcodec_parameters_from_context(m_video->codecpar, enc),
        "encoder params");
    m_video->time_base = enc->time_base;

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

void FileMuxSink::on_packet(AVPacket* pkt, AVRational enc_time_base)
{
    av_packet_rescale_ts(pkt, enc_time_base, m_video->time_base);
    pkt->stream_index = m_video->index;
    roiav::check(av_interleaved_write_frame(m_out.p, pkt), "write video");
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
