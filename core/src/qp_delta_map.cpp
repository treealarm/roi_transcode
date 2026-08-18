#include "qp_delta_map.h"

#include <algorithm>

QpDeltaMap QpDeltaMapBuilder::build(
    int width,
    int height,
    int block,
    const std::vector<RoiRect>& rects) const
{
    QpDeltaMap m;
    if (width <= 0 || height <= 0 || block <= 0) {
        return m;
    }
    m.block = block;
    m.cols = (width + block - 1) / block;
    m.rows = (height + block - 1) / block;
    m.deltas.assign(static_cast<size_t>(m.cols) * static_cast<size_t>(m.rows), 0);

    // Paint last→first so index 0 overwrites (Intel/FFmpeg: first region wins).
    for (int i = static_cast<int>(rects.size()) - 1; i >= 0; --i) {
        const RoiRect& r = rects[static_cast<size_t>(i)];
        if (r.right <= r.left || r.bottom <= r.top) {
            continue;
        }
        const int8_t qp = static_cast<int8_t>(std::clamp(r.qp_delta, -51, 51));
        const int c0 = std::clamp(r.left / block, 0, m.cols - 1);
        const int c1 = std::clamp((r.right - 1) / block, 0, m.cols - 1);
        const int r0 = std::clamp(r.top / block, 0, m.rows - 1);
        const int r1 = std::clamp((r.bottom - 1) / block, 0, m.rows - 1);
        for (int y = r0; y <= r1; ++y) {
            for (int x = c0; x <= c1; ++x) {
                m.deltas[static_cast<size_t>(y * m.cols + x)] = qp;
            }
        }
    }
    return m;
}
