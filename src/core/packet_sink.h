#pragma once

// Where encoded packets go. The encoder does not know whether it is filling a
// file or handing bytes back to a caller, which is the whole point: the frame
// mode needs the same encoder without a muxer behind it.

#include "av_raii.h"

#include <functional>
#include <string>
#include <vector>
namespace smartvideo {


class PacketSink {
public:
    virtual ~PacketSink() = default;

    // Called once, after the encoder is open and before any packet arrives.
    virtual void begin(AVCodecContext* enc) = 0;
    // `pkt` is in `enc_time_base` and is consumed by the sink.
    virtual void on_packet(AVPacket* pkt, AVRational enc_time_base) = 0;
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

    void begin(AVCodecContext* enc) override;
    void on_packet(AVPacket* pkt, AVRational enc_time_base) override;
    void end() override;

private:
    std::string m_path;
    std::string m_movflags;
    roiav::FmtOut m_out;
    AVStream* m_video = nullptr;
    AVStream* m_audio = nullptr;
    const AVCodecParameters* m_audio_par = nullptr;
    AVRational m_audio_tb{};
    bool m_started = false;
    bool m_finished = false;
};

// Hands packets to a callback instead of a file.
class CallbackSink : public PacketSink {
public:
    using OnPacket = std::function<void(const AVPacket*, AVRational)>;
    // `on_open` reports the opened encoder so a caller can read codecpar out of
    // it -- a remote consumer needs the extradata before the first packet.
    using OnOpen = std::function<void(AVCodecContext*)>;

    explicit CallbackSink(OnPacket on_packet, OnOpen on_open = {})
        : m_on_packet(std::move(on_packet)), m_on_open(std::move(on_open))
    {
    }

    void begin(AVCodecContext* enc) override
    {
        if (m_on_open) {
            m_on_open(enc);
        }
    }
    void on_packet(AVPacket* pkt, AVRational tb) override
    {
        if (m_on_packet) {
            m_on_packet(pkt, tb);
        }
    }
    void end() override {}

private:
    OnPacket m_on_packet;
    OnOpen m_on_open;
};

} // namespace smartvideo
