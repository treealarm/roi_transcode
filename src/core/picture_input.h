#pragma once

// Turns what a caller pushes into AVFrames. Raw planes are wrapped without a
// copy; H.264, H.265 and JPEG go through a decoder.

#include "av_handles.h"
#include "ta_roienc/transcoder.h"

#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace ta_roienc {

class PictureDecoder {
public:
    using PictureCb = std::function<void(AVFrame*)>;

    // `extradata` may be null; for H.264/H.265 in AVCC/HVCC form it is required
    // for the decoder to make sense of the first access unit.
    //
    // `framing` says whether every push carries one complete access unit. It decides whether
    // a parser goes in front of the decoder, and a parser is not free: it holds each picture
    // back until the start of the next one arrives, because that is the only thing that ends
    // an Annex-B access unit. One picture of delay is nothing on a stream decoded whole, and
    // a whole GOP on a caller that pushes only keyframes -- which is what analytics does.
    //
    // `hw_device` is the encoder's own device, or null to decode on the CPU. Given one, the
    // decoder produces frames that already live in the encoder's memory and SubmitAvPicture
    // hands them straight over -- no conversion, no upload, no copy. Given one it cannot
    // use (a codec the device has no decoder for, a profile the silicon does not implement)
    // it decodes in software instead and says so through hardware_name(): a hardware path
    // that quietly became a software one would be a silent fallback.
    enum class Framing {
        /// Buffers may split an access unit or carry several; a parser reassembles them.
        Arbitrary,
        /// Every push is exactly one access unit, so nothing has to be reassembled.
        CompleteAccessUnits,
    };

    void open(ta_roienc::SourceCodec codec, const uint8_t* extradata, int extradata_size,
        AVBufferRef* hw_device = nullptr, Framing framing = Framing::Arbitrary);

    // Which libavcodec decoder opened ("hevc", "h264", "mjpeg"); empty for a raw session.
    const std::string& decoder_name() const { return m_decoder_name; }

    // The hardware the frames actually came back on ("vaapi", "qsv"), empty when the decode
    // ran on the CPU. Read from the negotiation rather than from what was asked for: the
    // format is settled on the first packet, and until then there is nothing to report but
    // an intention.
    const std::string& hardware_name() const { return m_hardware_name; }

    // Feeds one buffer. Calls `cb` for every frame that comes out -- usually one,
    // sometimes none while the decoder builds up state. Reads [data, data + size) and nothing
    // past it, and keeps nothing of it after returning.
    void push(const uint8_t* data, size_t size, int64_t pts, const PictureCb& cb);
    // Drains frames the parser and the decoder are still holding.
    void flush(const PictureCb& cb);

private:
    void receive(const PictureCb& cb);
    // Sends one access unit to the decoder and hands on whatever that releases.
    void decode(const uint8_t* data, int size, int64_t pts, const PictureCb& cb);
    // decode() for an access unit the parser emitted, with the parser's timestamp for it.
    void decode_parsed(const uint8_t* data, int size, const PictureCb& cb);
    // Fills m_dec for `codec`, optionally on `hw_device`. Returns false if the context
    // could not be opened, leaving nothing behind for the caller to clean up.
    bool open_context(const AVCodec* dec, const uint8_t* extradata, int extradata_size,
        AVBufferRef* hw_device);
    // libavcodec's get_format: picks the hardware format when the decoder offers it.
    static AVPixelFormat choose_format(AVCodecContext* ctx, const AVPixelFormat* formats);

    ta_roienc::SourceCodec m_codec = ta_roienc::SourceCodec::Raw;
    roiav::CodingCtx m_dec;
    AVCodecParserContext* m_parser = nullptr;
    // What the parser reads: the caller's bytes followed by the zeroed padding it reads past
    // their end. Kept between pushes so that its allocation is not repeated for each one.
    std::vector<uint8_t> m_padded;
    roiav::Picture m_frame;
    roiav::RawUnit m_pkt;

    std::string m_decoder_name;
    std::string m_hardware_name;
    // What choose_format should pick when libavcodec offers it, and what it settled on.
    AVPixelFormat m_hw_pix = AV_PIX_FMT_NONE;
    AVHWDeviceType m_hw_type = AV_HWDEVICE_TYPE_NONE;

public:
    ~PictureDecoder();
    PictureDecoder() = default;
    PictureDecoder(const PictureDecoder&) = delete;
    PictureDecoder& operator=(const PictureDecoder&) = delete;
};

} // namespace ta_roienc
