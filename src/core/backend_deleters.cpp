// The deleters behind SessionPtr and FrameBufferPtr.
//
// Compiled out of line on purpose: this is the library's side of the ABI, so the library's
// allocator frees what it allocated. Inlining them into the caller is exactly what must
// not happen.
//
// In the core rather than in backend_common.cpp: roi_encoder holds a SessionPtr and every
// backend returns one, so the definitions cannot live in an optional component a build is
// allowed not to have.

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
