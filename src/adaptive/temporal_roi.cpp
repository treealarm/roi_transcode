#include "temporal_roi.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace smartvideo {

namespace {

/// Linear blend of two samples of one track. `w` is 0 at `a` and 1 at `b`.
Box blend(const Box& a, const Box& b, double w)
{
    const float f = static_cast<float>(std::clamp(w, 0.0, 1.0));
    auto mix = [f](float lo, float hi) { return lo + (hi - lo) * f; };

    Box out = a;
    out.x = mix(a.x, b.x);
    out.y = mix(a.y, b.y);
    out.width = mix(a.width, b.width);
    out.height = mix(a.height, b.height);
    out.confidence = mix(a.confidence, b.confidence);
    return out;
}

double median_interval(const std::vector<Box>& samples)
{
    if (samples.size() < 2) {
        return 0.0;
    }
    std::vector<double> gaps;
    gaps.reserve(samples.size() - 1);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const double d = samples[i].t_sec - samples[i - 1].t_sec;
        if (d > 0.0) {
            gaps.push_back(d);
        }
    }
    if (gaps.empty()) {
        return 0.0;
    }
    // Median, not mean: a track that was interrupted once should not have its nominal rate
    // dragged out by the interruption -- which is precisely the thing being measured against.
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    return gaps[gaps.size() / 2];
}

} // namespace

TemporalRoi::TemporalRoi(std::span<const Box> boxes, const TemporalROIConfig& config,
    double fallback_hold_sec)
    : m_config(config), m_fallback_hold_sec(fallback_hold_sec)
{
    // Keyed by kind as well as id: track ids are unique per producer and per session, and a
    // detector that reports a person and a face may well number them independently.
    std::map<std::pair<uint64_t, std::string>, std::vector<Box>> grouped;
    for (const Box& b : boxes) {
        if (b.track_id == 0) {
            m_untracked.push_back(b);
            continue;
        }
        grouped[{b.track_id, b.kind}].push_back(b);
    }

    m_tracks.reserve(grouped.size());
    for (auto& [key, samples] : grouped) {
        std::sort(samples.begin(), samples.end(),
            [](const Box& l, const Box& r) { return l.t_sec < r.t_sec; });
        Track t;
        t.interval = median_interval(samples);
        t.samples = std::move(samples);
        m_tracks.push_back(std::move(t));
    }
}

double TemporalRoi::inferred_interval(std::size_t track_index) const
{
    return track_index < m_tracks.size() ? m_tracks[track_index].interval : 0.0;
}

std::vector<Box> TemporalRoi::at(double t_sec) const
{
    std::vector<Box> out;
    out.reserve(m_tracks.size() + m_untracked.size());

    for (const Track& track : m_tracks) {
        const auto& s = track.samples;
        // How long this track may go unseen before it is considered gone, in its own units.
        // max_missing_frames is a count of the producer's frames, and the producer's frame
        // rate is the interval measured above -- so one setting fits every source.
        // Patience measured in the producer's frames, capped in seconds -- see max_hold_sec.
        // The cap applies only to what is inferred from the interval. A caller that set
        // hold_sec said how long a box of theirs applies, and that is a decision rather than a
        // guess to be second-guessed -- the same rule a configured kind gets in the quality
        // table.
        double reach = m_fallback_hold_sec;
        if (track.interval > 0.0) {
            reach = track.interval * static_cast<double>(m_config.max_missing_frames);
            if (m_config.max_hold_sec > 0.0) {
                reach = std::min(reach, m_config.max_hold_sec);
            }
        }

        const auto after = std::lower_bound(s.begin(), s.end(), t_sec,
            [](const Box& b, double t) { return b.t_sec < t; });

        if (after == s.begin()) {
            // Before the track existed. Extended backwards for the same reason it is
            // extended forwards: the object was there before the detector first said so.
            const double ahead = s.front().t_sec - t_sec;
            if (ahead <= reach) {
                Box b = s.front();
                b.confidence *= std::pow(m_config.confidence_decay,
                    track.interval > 0 ? ahead / track.interval : 0.0);
                out.push_back(b);
            }
            continue;
        }
        if (after == s.end()) {
            // Past the last sample. This is the case hold_sec cannot distinguish from a gap,
            // and the one where being generous is wrong: the object may simply have left.
            const double behind = t_sec - s.back().t_sec;
            if (behind <= reach) {
                Box b = s.back();
                b.confidence *= std::pow(m_config.confidence_decay,
                    track.interval > 0 ? behind / track.interval : 0.0);
                out.push_back(b);
            }
            continue;
        }

        // Between two known samples. In an archive both ends are already recorded, so the
        // position is interpolated and is where the object actually was -- there is nothing
        // to predict and nothing to get wrong. A gap wider than the track is allowed to go
        // unseen is not bridged: that is two visits, not one.
        const Box& b1 = *after;
        const Box& b0 = *(after - 1);
        const double span = b1.t_sec - b0.t_sec;
        if (span > reach && span > 0.0) {
            continue;
        }
        out.push_back(span > 0.0 ? blend(b0, b1, (t_sec - b0.t_sec) / span) : b0);
    }

    // No track to stitch, so the old symmetric window is all there is.
    for (const Box& b : m_untracked) {
        if (std::abs(b.t_sec - t_sec) <= m_fallback_hold_sec) {
            out.push_back(b);
        }
    }
    return out;
}

} // namespace smartvideo
