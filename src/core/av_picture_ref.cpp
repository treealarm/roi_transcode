#include "ta_roienc/picture.h"

#include "engine/common/av_picture_store.h"

#include <memory>

// Turning an AVFrame into a Picture, and nothing else.
//
// Split out of av_picture_bridge.cpp, and the split is about what a link pulls in rather
// than about tidiness. SubmitAvPicture names VideoEncoder, which names the backend
// registry, which needs RegisterDeploymentEngines -- so an object file holding both the
// adapter and the submit helper drags the whole encoder into any program that only wanted
// to turn a decoded frame into a Picture. The standalone decoder is exactly that program,
// and an analytics worker that encodes nothing should not be linking encoder backends to
// read pixels. Static archives pull whole objects, so the fix is a second object.

namespace ta_roienc {

namespace {

/// An owned AVFrame behind the backend-neutral interface. AvPictureStore already knows how
/// to own one -- CreatePicture relies on it -- so the adapter is a construction, not a new
/// implementation.
std::shared_ptr<IPictureStore> own_av_frame(AVFrame* frame)
{
    auto buffer = std::make_shared<AvPictureStore>(nullptr);
    buffer->adopt(frame, nullptr, MemoryKind::AVFRAME);
    return buffer;
}

} // namespace

Picture AvPictureBridge::Ref(const AVFrame* frame)
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
    return Picture(own_av_frame(copy));
}

Picture AvPictureBridge::Adopt(AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    return Picture(own_av_frame(frame));
}

} // namespace ta_roienc
