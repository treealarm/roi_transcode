#pragma once

// The HEVC encoder with region-of-interest side data, independent of where the
// frames came from. A file pipeline and a caller pushing frames want exactly
// this and nothing else around it.

#include "av_raii.h"
#include "roi/types.h"
#include "roi/types.h"

#include <string>
#include <vector>
namespace roi {


class PacketSink;

struct EncoderParams {
    int width = 0;
    int height = 0;
    // Frame rate. Also picks the default GOP (two seconds) when gop == 0.
    AVRational fps{25, 1};
    // Encoder time base. Left at den == 0 it follows the frame rate, which is
    // what a file wants; a pushed source usually has its own units instead.
    AVRational time_base{0, 0};
    Encoder kind = Encoder::Auto;
    int crf = 28;
    int gop = 0;
    std::string preset = "medium";
};

class RoiEncoder {
public:
    // Opens a backend, falling back down the chain when kind is Auto, then
    // calls sink.begin(). Throws if nothing opens.
    void open(const EncoderParams& params, PacketSink& sink);

    // The same in two steps, for a caller that must know which backend opened
    // before the sink is told about it.
    void open_deferred(const EncoderParams& params);
    void attach(PacketSink& sink);

    Encoder kind() const { return m_kind; }
    // Human-readable name of the backend that actually opened.
    const char* label() const;
    AVCodecContext* ctx() { return m_enc.p; }
    AVRational time_base() const { return m_enc.p->time_base; }

    // `frame` may be in any software pixel format and is converted to what the
    // encoder wants; `pts` is already in the encoder time base. `rects` are in
    // pixels, strongest first -- on overlap the lowest index wins.
    void encode(AVFrame* frame, const std::vector<Rect>& rects, int64_t pts);
    // Drains the encoder. Does not end the sink.
    void flush();

private:
    void drain();
    AVFrame* to_encoder_format(AVFrame* src);

    Encoder m_kind = Encoder::X265;
    roiav::CodecCtx m_enc;
    roiav::Buffer m_hw_device;
    roiav::Buffer m_hw_frames;
    AVPixelFormat m_sw_pix = AV_PIX_FMT_YUV420P;
    bool m_hw_upload = false;

    roiav::Sws m_sws;
    int m_sws_src_w = 0;
    int m_sws_src_h = 0;
    int m_sws_src_fmt = -1;
    roiav::Frame m_sw_frame;
    bool m_sw_frame_ready = false;
    roiav::Packet m_pkt;
    // One frame's worth of the encoder time base, used when the encoder leaves
    // a packet's duration unset.
    int64_t m_frame_duration = 0;
    PacketSink* m_sink = nullptr;
};

} // namespace roi
