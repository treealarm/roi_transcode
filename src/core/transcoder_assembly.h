#pragma once

// Building a VideoEncoder against a deployment the caller assembled.
//
// RoiCoder takes a EngineCatalog the caller built; this is the same door one level up,
// for the facade.
//
// It is deliberately not in include/. The public entry point is CreateVideoEncoder(config)
// and it stays one function: the registry is not something a caller of the library chooses,
// it is something whoever assembles the deployment chooses, and that is this project. What
// needs the door is the test suite -- the branches where a backend throws, reports a
// clamped quantiser, or has no rate control of its own cannot be reached through libx264,
// which does none of those things on demand. A switch inside the shipping encoder to make
// it fail would be the alternative, and that is worse: it would be a fault path that only
// exists because it is tested.
//
// Header, not a friend declaration: the assembly is a function, not a privilege, and it is
// how CreateVideoEncoder itself is written.

#include "ta_roienc/encode.h"

#include <memory>

namespace ta_roienc {

/// Exactly CreateVideoEncoder, except that the backends come from `registry` rather than
/// from the process-wide one. Same validation, same refusals, same exceptions.
///
/// `registry` must outlive the encoder this returns: it is held by pointer all the way
/// down to RoiCoder, and every backend it names is consulted again on each open.
std::unique_ptr<VideoEncoder> CreateVideoEncoderIn(
    const EncodeOptions& config, const EngineCatalog& registry);

} // namespace ta_roienc
