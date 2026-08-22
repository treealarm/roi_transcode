// Block map to encoder regions.
//
// The last step before a backend: merge equal-delta blocks into rectangles, order them so
// the strongest survive the region budget, and drop the rest. Which budget applies is the
// backend's answer, not a setting -- see EncoderCapabilities::max_roi_regions.

#include "smartvideo/geometry.h"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace roi {

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
