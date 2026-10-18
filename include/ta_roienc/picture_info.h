#pragma once

// What the encoder is told about a frame besides its pixels.
//
// This travels beside the frame rather than inside it: an AVFrame belongs to whoever
// decoded it and has nowhere to put a detector's answer, and a library that stuffed its
// own data into one would be writing into somebody else's structure.

#include "ta_roienc/shapes.h"
#include "ta_roienc/tracked_object.h"
#include "ta_roienc/stamp.h"
#include "ta_roienc/basics.h"

#include <span>

namespace ta_roienc {

// What to keep sharp in this frame: a mask, boxes, a ready qp_offsets, or neither.
//
// Both carry their own length. The previous shape was a pointer and a count the caller
// filled in separately, which meant a wrong count read past the end of their buffer and
// nothing here could tell.
//
// Precedence when more than one form is filled: qp_offsets, then mask, then boxes. A detector
// that already painted the block grid should not be re-quantized through the mask path.
struct Regions {
    /// Takes precedence over `mask` and `boxes` when `deltas` is non-empty.
    QpOffsets qp_offsets;
    /// Takes precedence over `boxes` when it holds a consistent mask.
    Mask mask;
    std::span<const Box> boxes;
};

/// One frame's instructions. Borrowed for the length of the submit call: `regions` holds
/// spans into the caller's storage, and nothing here is kept afterwards.
struct PictureInfo {
    /// When this frame belongs. A timestamp that does not say its own units is how a
    /// stream ends up a factor of a thousand out and looking like working code.
    Stamp timestamp;

    Regions regions;

    /// Ask for this frame to be coded as a keyframe. Best effort, and reported through
    /// Counters::keyframes_requested -- the same thing RequestIdr does, for
    /// a caller that already knows at submit time.
    bool force_key_frame = false;
};

} // namespace ta_roienc
