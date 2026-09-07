#include "sve/frame.h"

#include "backend/common/av_frame_buffer.h"

#include <memory>

// Turning an AVFrame into a Frame, and nothing else.
//
// Split out of av_frame_adapter.cpp, and the split is about what a link pulls in rather
// than about tidiness. SubmitAvFrame names SmartVideoEncoder, which names the backend
// registry, which needs RegisterDeploymentBackends -- so an object file holding both the
// adapter and the submit helper drags the whole encoder into any program that only wanted
// to turn a decoded frame into a Frame. The standalone decoder is exactly that program,
// and an analytics worker that encodes nothing should not be linking encoder backends to
// read pixels. Static archives pull whole objects, so the fix is a second object.

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

} // namespace sve
