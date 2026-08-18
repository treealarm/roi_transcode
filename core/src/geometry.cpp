#include "roi/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
namespace roi {


namespace {

// Quantizes 0..255 to one of `levels` steps and interpolates the QP delta
// between the background and the strongest ROI value.
int delta_for_value(int value, const MaskMapping& mapping)
{
    const int levels = std::max(mapping.levels, 2);
    const int bucket =
        std::clamp(value * levels / 256, 0, levels - 1);
    const double pos = static_cast<double>(bucket) / (levels - 1);
    const double d = mapping.qp_delta_at_0
        + (mapping.qp_delta_at_255 - mapping.qp_delta_at_0) * pos;
    return static_cast<int>(std::lround(d));
}

} // namespace

QpDeltaMap qp_map_from_mask(const Mask& mask, const MaskMapping& mapping,
    int frame_width, int frame_height, int block)
{
    QpDeltaMap m;
    if (frame_width <= 0 || frame_height <= 0 || block <= 0 || !mask.data
        || mask.width <= 0 || mask.height <= 0) {
        return m;
    }
    m.block = block;
    m.width = frame_width;
    m.height = frame_height;
    m.cols = (frame_width + block - 1) / block;
    m.rows = (frame_height + block - 1) / block;
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
                const uint8_t* row = mask.data + static_cast<size_t>(y) * stride;
                for (int x = mx0; x < mx1 && x < mask.width; ++x) {
                    peak = std::max(peak, static_cast<int>(row[x]));
                }
            }
            const int delta = delta_for_value(peak, mapping);
            m.deltas[static_cast<size_t>(by) * m.cols + bx] =
                static_cast<int8_t>(std::clamp(delta, -51, 51));
        }
    }
    return m;
}

std::vector<Rect> rects_from_qp_map(
    const QpDeltaMap& map, int max_regions, int* dropped)
{
    std::vector<Rect> out;
    if (dropped) {
        *dropped = 0;
    }
    if (map.cols <= 0 || map.rows <= 0 || map.deltas.empty()) {
        return out;
    }

    // Horizontal runs of one delta, then glue a run to the identical run above
    // it. Two passes beat a general connected-components sweep here: the result
    // has to be rectangles anyway, so an L-shaped region must split regardless.
    struct Open {
        int col_begin;
        int col_end; // exclusive
        int row_begin;
        int delta;
    };
    std::vector<std::pair<Open, int>> done; // finished run + its row_end

    std::vector<Open> prev;
    for (int y = 0; y < map.rows; ++y) {
        std::vector<Open> cur;
        int x = 0;
        while (x < map.cols) {
            const int8_t d = map.deltas[static_cast<size_t>(y) * map.cols + x];
            int x2 = x + 1;
            while (x2 < map.cols
                && map.deltas[static_cast<size_t>(y) * map.cols + x2] == d) {
                ++x2;
            }
            if (d != 0) {
                cur.push_back(Open{x, x2, y, d});
            }
            x = x2;
        }

        // Extend runs from the previous row that match exactly.
        for (auto& c : cur) {
            for (auto& p : prev) {
                if (p.col_begin == c.col_begin && p.col_end == c.col_end
                    && p.delta == c.delta) {
                    c.row_begin = p.row_begin;
                    p.delta = 0; // consumed
                    break;
                }
            }
        }
        for (const auto& p : prev) {
            if (p.delta != 0) {
                done.emplace_back(p, y);
            }
        }
        prev = std::move(cur);
    }
    for (const auto& p : prev) {
        if (p.delta != 0) {
            done.emplace_back(p, map.rows);
        }
    }

    out.reserve(done.size());
    for (const auto& [run, row_end] : done) {
        Rect r;
        r.left = run.col_begin * map.block;
        r.right = std::min(run.col_end * map.block, map.width);
        r.top = run.row_begin * map.block;
        r.bottom = std::min(row_end * map.block, map.height);
        r.qp_delta = run.delta;
        if (r.right > r.left && r.bottom > r.top) {
            out.push_back(r);
        }
    }

    // Ascending delta: quality boosts first, strongest first, then background
    // suppression. Ordering by magnitude instead would rank a +20 background
    // above a -8 object -- the encoder keeps the lowest index on overlap, and
    // the truncated tail has to be the regions whose loss costs least. Dropping
    // a suppression only leaves that area at the base QP; dropping a boost
    // damages the object the whole exercise is meant to protect.
    std::stable_sort(out.begin(), out.end(),
        [](const Rect& a, const Rect& b) {
            return a.qp_delta < b.qp_delta;
        });

    if (max_regions > 0 && static_cast<int>(out.size()) > max_regions) {
        if (dropped) {
            *dropped = static_cast<int>(out.size()) - max_regions;
        }
        out.resize(static_cast<size_t>(max_regions));
    }
    return out;
}

} // namespace roi
