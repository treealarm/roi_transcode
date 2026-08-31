#pragma once

// Where encoded packets go. The encoder does not know whether it is filling a
// file or handing bytes back to a caller, which is the whole point: the frame
// mode needs the same encoder without a muxer behind it.

#include "av_raii.h"

#include "sve/codec_parameters.h"
#include "sve/packet.h"

#include <functional>
#include <string>
#include <vector>
namespace sve {


class PacketSink {
public:
    virtual ~PacketSink() = default;

    // Called once, after the encoder is open and before any packet arrives.
    //
    // CodecParameters rather than an AVCodecContext, and that is the change TZ v4.1 §20
    // forces. A sink taking a context could only ever be fed by a backend that had one,
    // which made this interface -- the one the whole file pipeline hangs off -- a
    // libavcodec interface wearing a neutral name. A backend on its own SDK could compile
    // and could not deliver a packet anywhere.
    virtual void begin(const CodecParameters& params) = 0;
    // The packet carries its own time base, so there is nothing to pass beside it and
    // nothing for a caller to get out of step with.
    virtual void on_packet(const EncodedPacket& packet) = 0;
    virtual void end() = 0;
};

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
    AVRational m_audio_tb{};
    bool m_started = false;
    bool m_finished = false;
    // Last DTS handed to the muxer, in the video stream's time base. Guards the monotonicity
    // the mp4 muxer requires; see on_packet.
    int64_t m_last_dts = AV_NOPTS_VALUE;
};

} // namespace sve
