#include "sve/frame.h"

#include "backend/common/av_frame_buffer.h"

#include <memory>

namespace sve {

namespace {

/// An owned AVFrame behind the backend-neutral interface. AvFrameBuffer already knows how
/// to own one -- CreateFrame relies on it -- so the adapter is a construction, not a new
/// implementation.
std::shared_ptr<IFrameBuffer> own_av_frame(AVFrame* frame)
{
    auto buffer = std::make_shared<AvFrameBuffer>(nullptr);
    buffer->adopt(frame, nullptr, MemoryType::AVFRAME);
    return buffer;
}

} // namespace

Frame AvFrameAdapter::Ref(const AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr) {
        return {};
    }
    if (av_frame_ref(copy, frame) < 0) {
        av_frame_free(&copy);
        return {};
    }
    return Frame(own_av_frame(copy));
}

Frame AvFrameAdapter::Adopt(AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    return Frame(own_av_frame(frame));
}

Status SubmitAvFrame(SmartVideoEncoder& encoder, const AVFrame* frame,
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

} // namespace sve
