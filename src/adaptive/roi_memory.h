#pragma once

// Keeping a region alive across a detector miss.
//
// An object is found on one sample, missed on the next, found again on the one after.
// Without something here the region blinks out and the encoder spends those frames treating
// the object as background.
//
// A symmetric hold window around every sample is blind in three ways at once, and all three
// are measurable on a real archive:
//
//   * It cannot tell a gap from an ending. The last box of an object that has walked out of
//     frame is extended exactly as eagerly as one in the middle of a track.
//   * One window cannot serve two sample rates. A camera's own ONVIF boxes arrive every
//     0.2s and the detector's every 1.0s; at a 0.5s hold the first gets five overlapping
//     copies of every object -- spending a region budget of 32 on six objects -- while the
//     second still leaves a one-second hole whenever the detector blinks.
//   * It holds the object at the position it was last seen, so a moving one drifts out of
//     its own rectangle.
//
// A track answers all three, and in an archive it answers them exactly. Both ends of a gap
// are already known, so the position between them is interpolated rather than predicted --
// no motion model, no guessing, and the result is where the object actually was. Prediction
// (use_track_prediction) applies only past the last sample (or before the first), within
// max_missing_frames patience -- that is the live-shaped case an archive still hits at the
// ends of a track.

#include "ta_roienc/rules.h"
#include "ta_roienc/basics.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ta_roienc {

class RoiMemory {
public:
    /// `boxes` is the whole timeline. Grouping happens once here rather than per frame.
    RoiMemory(std::span<const Box> boxes, const RoiPersistence& config,
        double fallback_hold_sec);

    /// The regions in force at `t_sec`: one per track, not one per sample.
    std::vector<Box> at(double t_sec) const;

    /// Nominal interval between samples of a track, inferred from the samples themselves.
    /// Zero when there was nothing to infer it from.
    double inferred_interval(std::size_t track_index) const;

    std::size_t track_count() const { return m_tracks.size(); }
    std::size_t untracked_count() const { return m_untracked.size(); }

private:
    struct Trail {
        std::vector<Box> samples;   // sorted by t_sec
        /// The rate this particular source runs at, measured rather than configured: the
        /// median interval between this track's own samples. It is what turns
        /// max_missing_frames into a duration, and it is why one setting can serve a 5 Hz
        /// camera and a 1 Hz detector at the same time.
        double interval = 0.0;
    };

    std::vector<Trail> m_tracks;
    /// Boxes with no track id -- nothing to stitch them into, so they keep the old
    /// behaviour. A source that reports no track is not a source we can be clever about.
    std::vector<Box> m_untracked;
    RoiPersistence m_config;
    double m_fallback_hold_sec;
};

} // namespace ta_roienc
