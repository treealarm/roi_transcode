#pragma once

// The PacketSink that writes a container -- TZ v4.1 §52's example application, not the
// library. Split out of packet_sink.h so that the interface every backend and the encoder
// itself include stays free of libavformat (§5.5).

#include "av_raii.h"
#include "packet_sink.h"

#include <string>
#include <vector>

namespace sve {

// Muxes into a container chosen by the output file extension.
class FileMuxSink : public PacketSink {
public:
    // `movflags` empty leaves the muxer default. ta_vms archive clips are
    // fragmented mp4 ("frag_keyframe+empty_moov+default_base_moof") and exports
    // use "faststart", so replacing a clip in place has to say which it wants.
    FileMuxSink(std::string path, std::string movflags = {});

    // Declares a stream-copied audio track. Call before begin(); the stream is
    // created there so video keeps index 0.
    void declare_audio(const AVCodecParameters* par, AVRational time_base);

    /// The input's display matrix, so the output is played the way the input was.
    ///
    /// Only the application that opened the input knows it, exactly as with the audio
    /// parameters above. A phone or a dashcam stores the picture in sensor orientation and
    /// puts the rotation in this side data; the decoder hands us the stored pixels, so
    /// dropping the matrix turns a transcode into a rotated copy of the original. Found on
    /// real footage (rotation=-180) on 2026-09-03 -- no synthetic fixture has one.
    void declare_display_matrix(const uint8_t* data, std::size_t size);
    bool has_audio() const { return m_audio_par != nullptr; }
    // `pkt` is in `src_time_base` and is consumed.
    void write_audio(AVPacket* pkt, AVRational src_time_base);

    void begin(const CodecParameters& params) override;
    void on_packet(const EncodedPacket& packet) override;
    void end() override;

private:
    void open_video_stream();
    void finish_begin();
    /// The muxer's own step: rescale, guard DTS monotonicity, write. Takes an
    /// AVPacket because libavformat does; nothing above this line does.
    void write_muxed(AVPacket* pkt, AVRational enc_time_base);

    std::string m_path;
    std::string m_movflags;
    roiav::FmtOut m_out;
    AVStream* m_video = nullptr;
    AVStream* m_audio = nullptr;
    const AVCodecParameters* m_audio_par = nullptr;
    /// A copy, not a pointer: the input's codecpar may be gone by the time begin() runs.
    std::vector<uint8_t> m_display_matrix;
    AVRational m_audio_tb{};
    bool m_started = false;
    bool m_finished = false;
    // Last DTS handed to the muxer, in the video stream's time base. Guards the monotonicity
    // the mp4 muxer requires; see on_packet.
    int64_t m_last_dts = AV_NOPTS_VALUE;
};

} // namespace sve
