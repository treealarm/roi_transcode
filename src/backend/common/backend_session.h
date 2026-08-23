#pragma once

// The libavcodec session: what BackendSession becomes for the four backends that reach
// hardware through FFmpeg. A backend built on an SDK directly derives its own instead, and
// shares none of these fields -- which is the reason BackendSession is a base and not a
// structure.

#include "av_raii.h"
#include "smartvideo/encoder_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace smartvideo {

class LibavSession final : public BackendSession {
public:
    Encoder kind = Encoder::Cpu;
    roiav::CodecCtx enc;
    roiav::Buffer hw_device;
    roiav::Buffer hw_frames;
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    /// The encoder takes hardware frames, so software frames must be uploaded first.
    bool hw_upload = false;

    /// Where ReceivePacket puts what it took. Owned here because EncodedPacket only
    /// borrows: the pointer it hands out is this packet's, valid until the next call.
    roiav::Packet pkt;

    /// Scratch for SubmitFrame: planes in system memory wrapped without a copy, the
    /// uploaded copy, and the converted one.
    roiav::Frame wrapper;
    roiav::Frame hw_frame;
    roiav::Frame sw_frame;
    bool sw_frame_ready = false;
    roiav::Sws sws;
    int sws_src_w = 0;
    int sws_src_h = 0;
    int sws_src_fmt = -1;

    /// How many frames this session had to convert or upload before the encoder would take
    /// them. Zero on both is what "zero-copy" means, and the only way to tell whether a
    /// factory frame actually avoided the copy rather than moved it.
    long frames_converted = 0;
    long frames_uploaded = 0;
};

/// The session as the backend that made it knows it. Every LibavBackend method starts
/// here, and the cast is safe because a backend only ever receives sessions it opened.
LibavSession& libav(BackendSession& session);

} // namespace smartvideo
