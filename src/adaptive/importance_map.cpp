// The importance map: what each block of the frame is worth.
//
// Two ways in, one result. A grayscale mask carries importance directly in its values;
// rectangles carry it as a per-region delta. Both land on the same block grid, because
// that is the granularity every encoder backend actually accepts.

#include "importance_map.h"
#include "ta_roienc/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ta_roienc {

namespace {

// Quantizes 0..255 to one of `levels` steps and interpolates the QP delta
// between the background and the strongest ROI value.
int delta_for_value(int value, const MaskMapping& mapping)
{
    const int levels = std::max(mapping.levels, 2);
    const int bucket =
        std::clamp(value * levels / kMaskValues, 0, levels - 1);
    const double pos = static_cast<double>(bucket) / (levels - 1);
    const double d = mapping.qp_delta_at_0
        + (mapping.qp_delta_at_255 - mapping.qp_delta_at_0) * pos;
    return static_cast<int>(std::lround(d));
}

} // namespace

QpDeltaMap qp_map_from_mask(const Mask& mask, const MaskMapping& mapping,
    int32_t frame_width, int32_t frame_height, int32_t block)
{
    QpDeltaMap m;
    if (frame_width <= 0 || frame_height <= 0 || block <= 0 || !mask.consistent()) {
        return m;
    }
    m.block = block;
    m.width = frame_width;
    m.height = frame_height;
    m.cols = qp_map_cols(frame_width, block);
    m.rows = qp_map_rows(frame_height, block);
    m.deltas.assign(static_cast<size_t>(m.cols) * static_cast<size_t>(m.rows), 0);

    const int stride = mask.stride > 0 ? mask.stride : mask.width;

    for (int by = 0; by < m.rows; ++by) {
        // Block bounds in frame pixels, then in mask pixels. The mask is
        // sampled, not resized: it may be any resolution.
        const int fy0 = by * block;
        const int fy1 = std::min(fy0 + block, frame_height);
        const int my0 = fy0 * mask.height / frame_height;
        const int my1 = std::max(my0 + 1, fy1 * mask.height / frame_height);

        for (int bx = 0; bx < m.cols; ++bx) {
            const int fx0 = bx * block;
            const int fx1 = std::min(fx0 + block, frame_width);
            const int mx0 = fx0 * mask.width / frame_width;
            const int mx1 = std::max(mx0 + 1, fx1 * mask.width / frame_width);

            int peak = 0;
            for (int y = my0; y < my1 && y < mask.height; ++y) {
                const uint8_t* row = mask.data.data() + static_cast<size_t>(y) * stride;
                for (int x = mx0; x < mx1 && x < mask.width; ++x) {
                    peak = std::max(peak, static_cast<int>(row[x]));
                }
            }
            const int delta = delta_for_value(peak, mapping);
            m.deltas[static_cast<size_t>(by) * m.cols + bx] =
                static_cast<int8_t>(std::clamp(delta, kMinQpDelta, kMaxQpDelta));
        }
    }
    return m;
}


QpDeltaMap QpDeltaMapBuilder::build(
    int width, int height, int block, std::span<const Rect> rects) const
{
    QpDeltaMap m;
    if (width <= 0 || height <= 0 || block <= 0) {
        return m;
    }
    m.block = block;
    m.width = width;
    m.height = height;
    m.cols = qp_map_cols(width, block);
    m.rows = qp_map_rows(height, block);
    m.deltas.assign(static_cast<size_t>(m.cols) * static_cast<size_t>(m.rows), 0);
    paint(width, height, block, rects, m.deltas);
    return m;
}

void QpDeltaMapBuilder::paint(int width, int height, int block,
    std::span<const Rect> rects, std::span<int8_t> out) const
{
    const int cols = qp_map_cols(width, block);
    const int rows = qp_map_rows(height, block);
    if (cols <= 0 || rows <= 0
        || out.size() < static_cast<size_t>(cols) * static_cast<size_t>(rows)) {
        return;
    }

    // Paint last-to-first so index 0 overwrites (Intel/FFmpeg: first region wins).
    for (int i = static_cast<int>(rects.size()) - 1; i >= 0; --i) {
        const Rect& r = rects[static_cast<size_t>(i)];
        if (r.right <= r.left || r.bottom <= r.top) {
            continue;
        }
        const int8_t qp = static_cast<int8_t>(std::clamp(r.qp_delta, kMinQpDelta, kMaxQpDelta));
        const int c0 = std::clamp(r.left / block, 0, cols - 1);
        const int c1 = std::clamp((r.right - 1) / block, 0, cols - 1);
        const int r0 = std::clamp(r.top / block, 0, rows - 1);
        const int r1 = std::clamp((r.bottom - 1) / block, 0, rows - 1);
        for (int y = r0; y <= r1; ++y) {
            for (int x = c0; x <= c1; ++x) {
                out[static_cast<size_t>(y * cols + x)] = qp;
            }
        }
    }
}

} // namespace ta_roienc
