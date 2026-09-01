// The deleters behind SessionPtr and FrameBufferPtr -- TZ v4.1 §21.
//
// Compiled out of line on purpose: this is the library's side of the ABI, so the library's
// allocator frees what it allocated. Inlining them into the caller is exactly what must
// not happen.
//
// They were in backend_common.cpp, which is the libavcodec backends' shared base and an
// optional component under §5.5. That put two definitions the public interface promises --
// roi_encoder holds a SessionPtr, every backend returns one -- inside a component a build
// is allowed not to have. Invisible while everything was one archive; the first
// FFmpeg-free profile refused to link.

#include "sve/encoder_backend.h"

namespace sve {

void SessionDeleter::operator()(BackendSession* session) const noexcept
{
    delete session;
}

void FrameBufferDeleter::operator()(IFrameBuffer* buffer) const noexcept
{
    delete buffer;
}

} // namespace sve
