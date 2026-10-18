#include "ta_roienc/picture.h"

// The encoder-facing half of the AVFrame interop: submitting an AVFrame, and reading back
// the device the encoder holds. Both name VideoEncoder.
//
// AvPictureBridge::Ref and ::Adopt are next door in av_picture_ref.cpp, deliberately: they
// name no encoder, and keeping them in a separate object is what lets the standalone
// decoder turn an AVFrame into a Picture without linking an encoder it never calls.

namespace ta_roienc {

Verdict SubmitAvPicture(VideoEncoder& encoder, const AVFrame* frame,
    const PictureInfo& metadata)
{
    if (frame == nullptr) {
        return Verdict::InvalidArgument("SubmitAvPicture needs a frame");
    }
    Picture held = AvPictureBridge::Ref(frame);
    if (!held) {
        return Verdict::OutOfMemory("could not reference the frame");
    }
    return encoder.SubmitPicture(StampedPicture{std::move(held), metadata});
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
