#include "temporal_roi.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace smartvideo {

namespace {

/// Centre of a normalised box.
void centre(const Box& b, float& cx, float& cy)
{
    cx = b.x + 0.5f * b.width;
    cy = b.y + 0.5f * b.height;
}

/// Speed between two samples in frame-diagonals per second. The unit square's diagonal is
/// √2, so a box that crosses the whole frame in one second scores 1.0.
float speed_between(const Box& a, const Box& b)
{
    const double dt = b.t_sec - a.t_sec;
    if (dt <= 0.0) {
        return 0.0f;
    }
    float ax, ay, bx, by;
    centre(a, ax, ay);
    centre(b, bx, by);
    const float dx = bx - ax;
    const float dy = by - ay;
    const float dist = std::sqrt(dx * dx + dy * dy);
    constexpr float diagonal = 1.41421356f; // √2
    return static_cast<float>(dist / (diagonal * dt));
}

TrackEvidence evidence_for(const std::vector<Box>& samples, double t_sec)
{
    TrackEvidence e;
    if (samples.empty()) {
        return e;
    }
    e.present = true;
    e.sample_count = static_cast<int32_t>(samples.size());
    e.age_sec = static_cast<float>(std::max(0.0, t_sec - samples.front().t_sec));

    if (samples.size() < 2) {
        return e;
    }
    // Prefer the pair that brackets t; otherwise the nearest neighbouring pair.
    const auto after = std::lower_bound(samples.begin(), samples.end(), t_sec,
        [](const Box& b, double t) { return b.t_sec < t; });
    if (after != samples.begin() && after != samples.end()) {
        e.speed = speed_between(*(after - 1), *after);
    } else if (after == samples.begin()) {
        e.speed = speed_between(samples[0], samples[1]);
    } else {
        e.speed = speed_between(samples[samples.size() - 2], samples.back());
    }
    return e;
}

/// Linear blend of two samples of one track. `w` is 0 at `a` and 1 at `b`.
/// Values outside [0,1] extrapolate -- that is what track prediction needs past the ends.
Box blend(const Box& a, const Box& b, double w)
{
    const float f = static_cast<float>(w);
    auto mix = [f](float lo, float hi) { return lo + (hi - lo) * f; };

    Box out = a;
    out.x = mix(a.x, b.x);
    out.y = mix(a.y, b.y);
    out.width = mix(a.width, b.width);
    out.height = mix(a.height, b.height);
    out.confidence = mix(a.confidence, b.confidence);
    // Confidence is not meaningful outside the segment; clamp it.
    out.confidence = std::clamp(out.confidence, 0.0f, 1.0f);
    return out;
}

/// Extrapolate past `last` using the velocity of the last segment. TZ v3 use_track_prediction.
Box predict_from(const std::vector<Box>& samples, double t_sec)
{
    Box out = samples.back();
    if (samples.size() < 2) {
        return out;
    }
    const Box& a = samples[samples.size() - 2];
    const Box& b = samples.back();
    const double span = b.t_sec - a.t_sec;
    if (span <= 0.0) {
        return out;
    }
    const double w = (t_sec - a.t_sec) / span; // > 1 when past b
    return blend(a, b, w);
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
                if (m_config.use_track_prediction && s.size() >= 2) {
                    const Box& a0 = s[0];
                    const Box& a1 = s[1];
                    const double span = a1.t_sec - a0.t_sec;
                    if (span > 0.0) {
                        // Same linear model as forward prediction, evaluated before a0.
                        b = blend(a0, a1, (t_sec - a0.t_sec) / span);
                    }
                }
                b.confidence = s.front().confidence;
                b.confidence *= std::pow(m_config.confidence_decay,
                    track.interval > 0 ? ahead / track.interval : 0.0);
                b.track = evidence_for(s, t_sec);
                out.push_back(b);
            }
            continue;
        }
        if (after == s.end()) {
            // Past the last sample. This is the case hold_sec cannot distinguish from a gap,
            // and the one where being generous is wrong: the object may simply have left.
            const double behind = t_sec - s.back().t_sec;
            if (behind <= reach) {
                Box b = (m_config.use_track_prediction && s.size() >= 2)
                    ? predict_from(s, t_sec)
                    : s.back();
                b.confidence = s.back().confidence;
                b.confidence *= std::pow(m_config.confidence_decay,
                    track.interval > 0 ? behind / track.interval : 0.0);
                b.track = evidence_for(s, t_sec);
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
        Box live = span > 0.0 ? blend(b0, b1, (t_sec - b0.t_sec) / span) : b0;
        live.track = evidence_for(s, t_sec);
        out.push_back(live);
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
