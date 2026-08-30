#include "box_smoother.h"

#include <algorithm>
#include <functional>
#include <string>

namespace sve {

uint64_t BoxSmoother::key_of(const Box& b)
{
    // Kind is part of the identity: person #7 and face #7 are not the same track.
    const uint64_t kind_hash = static_cast<uint64_t>(std::hash<std::string>{}(b.kind));
    return b.track_id ^ (kind_hash << 1);
}

void BoxSmoother::apply(std::vector<Box>& boxes, float temporal, float spatial)
{
    temporal = std::clamp(temporal, 0.0f, 1.0f);
    spatial = std::clamp(spatial, 0.0f, 1.0f);
    if (temporal <= 0.0f && spatial <= 0.0f) {
        return;
    }

    for (Box& b : boxes) {
        if (b.track_id == 0) {
            continue;
        }
        const uint64_t key = key_of(b);
        const auto it = m_state.find(key);
        if (it == m_state.end()) {
            m_state.emplace(key, State{b.x, b.y, b.width, b.height});
            continue;
        }
        State& s = it->second;
        if (temporal > 0.0f) {
            b.x = temporal * b.x + (1.0f - temporal) * s.x;
            b.y = temporal * b.y + (1.0f - temporal) * s.y;
        }
        if (spatial > 0.0f) {
            b.width = spatial * b.width + (1.0f - spatial) * s.width;
            b.height = spatial * b.height + (1.0f - spatial) * s.height;
        }
        s = State{b.x, b.y, b.width, b.height};
    }
}

} // namespace sve
