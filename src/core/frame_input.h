#pragma once

// Turns what a caller pushes into AVFrames. Raw planes are wrapped without a
// copy; H.264, H.265 and JPEG go through a decoder.

#include "av_raii.h"
#include "sve/session.h"

#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace sve {

class FrameDecoder {
public:
    using FrameCb = std::function<void(AVFrame*)>;

    // `extradata` may be null; for H.264/H.265 in AVCC/HVCC form it is required
    // for the decoder to make sense of the first access unit.
    //
    // `hw_device` is the encoder's own device, or null to decode on the CPU. Given one, the
    // decoder produces frames that already live in the encoder's memory and SubmitAvFrame
    // hands them straight over -- no conversion, no upload, no copy. Given one it cannot
    // use (a codec the device has no decoder for, a profile the silicon does not implement)
    // it decodes in software instead and says so through hardware_name(), because a
    // hardware path that quietly became a software one is what §50 forbids.
    void open(sve::InputCodec codec, const uint8_t* extradata, int extradata_size,
        AVBufferRef* hw_device = nullptr);

    // Which libavcodec decoder opened ("hevc", "h264", "mjpeg"); empty for a raw session.
    const std::string& decoder_name() const { return m_decoder_name; }

    // The hardware the frames actually came back on ("vaapi", "qsv"), empty when the decode
    // ran on the CPU. Read from the negotiation rather than from what was asked for: the
    // format is settled on the first packet, and until then there is nothing to report but
    // an intention.
    const std::string& hardware_name() const { return m_hardware_name; }

    // Feeds one buffer. Calls `cb` for every frame that comes out -- usually one,
    // sometimes none while the decoder builds up state.
    void push(const uint8_t* data, size_t size, int64_t pts, const FrameCb& cb);
    // Drains frames the decoder is still holding.
    void flush(const FrameCb& cb);

private:
    void receive(const FrameCb& cb);
    // Fills m_dec for `codec`, optionally on `hw_device`. Returns false if the context
    // could not be opened, leaving nothing behind for the caller to clean up.
    bool open_context(const AVCodec* dec, const uint8_t* extradata, int extradata_size,
        AVBufferRef* hw_device);
    // libavcodec's get_format: picks the hardware format when the decoder offers it.
    static AVPixelFormat choose_format(AVCodecContext* ctx, const AVPixelFormat* formats);

    sve::InputCodec m_codec = sve::InputCodec::Raw;
    roiav::CodecCtx m_dec;
    AVCodecParserContext* m_parser = nullptr;
    roiav::Frame m_frame;
    roiav::Packet m_pkt;

    std::string m_decoder_name;
    std::string m_hardware_name;
    // What choose_format should pick when libavcodec offers it, and what it settled on.
    AVPixelFormat m_hw_pix = AV_PIX_FMT_NONE;
    AVHWDeviceType m_hw_type = AV_HWDEVICE_TYPE_NONE;

public:
    ~FrameDecoder();
    FrameDecoder() = default;
    FrameDecoder(const FrameDecoder&) = delete;
    FrameDecoder& operator=(const FrameDecoder&) = delete;
};

} // namespace sve
