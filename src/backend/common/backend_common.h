#pragma once

// Bits every backend's Configure() needs: the geometry, timing and GOP that do not depend
// on which encoder opened.

#include "backend/common/encoder_backend.h"

namespace smartvideo::backend_common {

/// Fills the parts of an AVCodecContext that are the same whoever encodes.
void apply_shared(AVCodecContext* enc, const EncoderParams& p);

/// Opens a hardware device, returning false rather than throwing when it is absent --
/// an absent GPU is the normal case on some machines, not an error.
bool open_hw_device(roiav::Buffer& hw, AVHWDeviceType type, const char* device);

} // namespace smartvideo::backend_common
