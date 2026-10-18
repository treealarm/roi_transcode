#pragma once

// Where encoded packets go. The encoder does not know whether it is filling a
// file or handing bytes back to a caller, which is the whole point: the frame
// mode needs the same encoder without a muxer behind it.
//
// This header names no FFmpeg type, and that is load-bearing rather than tidy. It is the
// interface the whole pipeline hangs off, so anything it includes, the core includes.
// FileMuxer -- the one implementation that does need libavformat -- lives next door in
// file_muxer.h.

#include "ta_roienc/coding_params.h"
#include "ta_roienc/access_unit.h"

namespace ta_roienc {

class AccessUnitOutlet {
public:
    virtual ~AccessUnitOutlet() = default;

    // Called once, after the encoder is open and before any packet arrives.
    //
    // CodingParams rather than an AVCodecContext: a sink taking a context could only
    // ever be fed by a backend that had one, which would make this interface -- the one the
    // whole file pipeline hangs off -- a libavcodec interface wearing a neutral name.
    virtual void begin(const CodingParams& params) = 0;
    // The packet carries its own time base, so there is nothing to pass beside it and
    // nothing for a caller to get out of step with.
    virtual void on_access_unit(const AccessUnit& packet) = 0;
    virtual void end() = 0;
};

} // namespace ta_roienc
