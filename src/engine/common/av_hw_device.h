#pragma once

// What a build of FFmpeg can reach, asked of libavutil itself.
//
// Shared by the decoder and the encoders because both have to tell "this FFmpeg has no such
// device" from "the device would not open": the two have different fixes, one in the build
// and one on the machine, and a backend that reports the second when it means the first
// sends whoever reads the log to the wrong place.

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace ta_roienc {

/// Whether this build of FFmpeg contains the device type at all.
bool hw_device_type_present(AVHWDeviceType type) noexcept;

} // namespace ta_roienc
