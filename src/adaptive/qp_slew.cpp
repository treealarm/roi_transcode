#include "qp_slew.h"

namespace sve {

uint64_t QpSlewLimiter::key_of(const Detection& d)
{
    if (d.track_id != 0) {
        return d.track_id;
    }
    // Untracked: identity is the pixel box. Good enough for a single-frame flicker; not a
    // substitute for a real track id.
    return (static_cast<uint64_t>(static_cast<uint32_t>(d.x1)) << 48)
        ^ (static_cast<uint64_t>(static_cast<uint32_t>(d.y1)) << 32)
        ^ (static_cast<uint64_t>(static_cast<uint32_t>(d.x2)) << 16)
        ^ static_cast<uint64_t>(static_cast<uint32_t>(d.y2));
}

void QpSlewLimiter::apply(std::vector<Detection>& detections, int32_t max_step)
{
    if (max_step <= 0) {
        return;
    }
    for (Detection& d : detections) {
        const uint64_t key = key_of(d);
        const auto it = m_last_qp.find(key);
        if (it != m_last_qp.end()) {
            const int32_t lo = it->second - max_step;
            const int32_t hi = it->second + max_step;
            if (d.qp_delta > hi) {
                d.qp_delta = hi;
            } else if (d.qp_delta < lo) {
                d.qp_delta = lo;
            }
            d.pad_qp_delta = d.qp_delta / 2;
        }
        m_last_qp[key] = d.qp_delta;
    }
}

} // namespace sve
