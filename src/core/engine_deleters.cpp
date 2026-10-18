// The deleters behind TranscoderPtr and PictureStorePtr.
//
// Compiled out of line on purpose: this is the library's side of the ABI, so the library's
// allocator frees what it allocated. Inlining them into the caller is exactly what must
// not happen.
//
// In the core rather than in engine_common.cpp: roi_coder holds a TranscoderPtr and every
// backend returns one, so the definitions cannot live in an optional component a build is
// allowed not to have.

#include "ta_roienc/engine.h"

namespace ta_roienc {

void TranscoderDeleter::operator()(EngineRun* session) const noexcept
{
    delete session;
}

void PictureStoreDeleter::operator()(IPictureStore* buffer) const noexcept
{
    delete buffer;
}

} // namespace ta_roienc
