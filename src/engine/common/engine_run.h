#pragma once

// The libavcodec session: what EngineRun becomes for the four backends that reach
// hardware through FFmpeg. A backend built on an SDK directly derives its own instead, and
// shares none of these fields -- which is the reason EngineRun is a base and not a
// structure.

#include "av_handles.h"
#include "ta_roienc/engine.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ta_roienc {

class LibavRun final : public EngineRun {
public:
    Engine kind = Engine::X264;
    roiav::CodingCtx enc;
    roiav::ByteBlock hw_device;
    roiav::ByteBlock hw_frames;
    /// The device a decoder in front of this encoder is handed, when it is not hw_device
    /// itself: the one QSV runs on top of (D3D11, VA-API), whose frames a decoder can
    /// produce and SubmitPicture maps into the encoder's pool. Null: hw_device is that device.
    roiav::ByteBlock decode_device;
    /// The pool those frames are mapped into, derived from the decoder's -- QSV unmaps a
    /// frame only through a pool that knows the one it came from -- and the decoder's pool
    /// it was derived from, to tell when the decoder has replaced it.
    roiav::ByteBlock mapped_frames;
    const uint8_t* mapped_from = nullptr;
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    /// The encoder takes hardware frames, so software frames must be uploaded first.
    bool hw_upload = false;

    /// Where ReceiveAccessUnit puts what it took. Owned here because AccessUnit only
    /// borrows: the pointer it hands out is this packet's, valid until the next call.
    roiav::RawUnit pkt;

    /// Scratch for SubmitPicture: planes in system memory wrapped without a copy, the
    /// uploaded copy, the converted one, and a decode_device frame mapped into the pool.
    roiav::Picture wrapper;
    roiav::Picture hw_frame;
    roiav::Picture mapped_frame;
    roiav::Picture sw_frame;
    bool sw_frame_ready = false;
    roiav::Scaler sws;
    int sws_src_w = 0;
    int sws_src_h = 0;
    int sws_src_fmt = -1;

    /// How many frames this session had to convert or upload before the encoder would take
    /// them. Zero on both is what "zero-copy" means, and the only way to tell whether a
    /// factory frame actually avoided the copy rather than moved it.
    long frames_converted = 0;
    long frames_uploaded = 0;
};

/// The session as the backend that made it knows it. Every LibavEngine method starts
/// here, and the cast is safe because a backend only ever receives sessions it opened.
LibavRun& libav(EngineRun& session);

} // namespace ta_roienc
