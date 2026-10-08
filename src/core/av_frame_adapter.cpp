#include "ta_roienc/frame.h"

// The encoder-facing half of the AVFrame interop: submitting an AVFrame, and reading back
// the device the encoder holds. Both name VideoEncoder.
//
// AvFrameAdapter::Ref and ::Adopt are next door in av_frame_ref.cpp, deliberately: they
// name no encoder, and keeping them in a separate object is what lets the standalone
// decoder turn an AVFrame into a Frame without linking an encoder it never calls.

namespace ta_roienc {

Status SubmitAvFrame(VideoEncoder& encoder, const AVFrame* frame,
    const FrameMetadata& metadata)
{
    if (frame == nullptr) {
        return Status::InvalidArgument("SubmitAvFrame needs a frame");
    }
    Frame held = AvFrameAdapter::Ref(frame);
    if (!held) {
        return Status::OutOfMemory("could not reference the frame");
    }
    return encoder.SubmitFrame(TimedFrame{std::move(held), metadata});
}

AVBufferRef* EncoderHardwareDevice(const VideoEncoder& encoder)
{
    const NativeHandle handle = encoder.HardwareDevice();
    if (handle.kind != NativeHandleKind::FfmpegHardwareDevice) {
        return nullptr;
    }
    return static_cast<AVBufferRef*>(handle.value);
}

} // namespace ta_roienc
