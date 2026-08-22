#include "object_normalizer.h"

#include <algorithm>
#include <cmath>

namespace roi {

namespace {

int clamp_i(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

void clamp_rect(Rect& r, int width, int height)
{
    r.left = clamp_i(r.left, 0, width);
    r.right = clamp_i(r.right, 0, width);
    r.top = clamp_i(r.top, 0, height);
    r.bottom = clamp_i(r.bottom, 0, height);
}

bool rect_valid(const Rect& r)
{
    return r.right > r.left && r.bottom > r.top;
}

KindQuality kind_or_default(const Quality& q, const std::string& kind)
{
    auto it = q.kinds.find(kind);
    return it != q.kinds.end() ? it->second : KindQuality{};
}

Detection to_detection(
    const Box& b, const KindQuality& kind, int width, int height)
{
    Detection d;
    d.x1 = static_cast<int>(std::lround(b.x * width));
    d.y1 = static_cast<int>(std::lround(b.y * height));
    d.x2 = static_cast<int>(std::lround((b.x + b.width) * width));
    d.y2 = static_cast<int>(std::lround((b.y + b.height) * height));
    d.qp_delta = kind.qp_delta;
    d.pad_qp_delta = kind.qp_delta / 2;
    d.pad_x = static_cast<int>(std::lround(kind.pad * width));
    d.pad_y = static_cast<int>(std::lround(kind.pad * height));
    return d;
}

} // namespace

std::vector<Rect> RoiMapBuilder::build(int width, int height,
    const std::vector<Detection>& detections, int background_qp_delta) const
{
    std::vector<Rect> out;
    if (width <= 0 || height <= 0) {
        return out;
    }

    // Lowest index wins on overlap (Intel VPL / FFmpeg QSV). Objects first.
    for (const auto& d : detections) {
        Rect obj{
            .top = d.y1,
            .bottom = d.y2,
            .left = d.x1,
            .right = d.x2,
            .qp_delta = d.qp_delta,
        };
        clamp_rect(obj, width, height);
        if (rect_valid(obj)) {
            out.push_back(obj);
        }

        Rect pad{
            .top = d.y1 - d.pad_y,
            .bottom = d.y2 + d.pad_y,
            .left = d.x1 - d.pad_x,
            .right = d.x2 + d.pad_x,
            .qp_delta = d.pad_qp_delta,
        };
        clamp_rect(pad, width, height);
        if (rect_valid(pad)) {
            out.push_back(pad);
        }
    }

    if (background_qp_delta != 0) {
        out.push_back(Rect{
            .top = 0,
            .bottom = height,
            .left = 0,
            .right = width,
            .qp_delta = background_qp_delta,
        });
    }

    return out;
}

std::vector<Detection> RoiMapBuilder::detections_at(
    const std::vector<Box>& boxes, const Quality& quality, int width,
    int height, double t_sec) const
{
    std::vector<Detection> out;
    const double hold = quality.hold_sec > 0 ? quality.hold_sec : 0.25;

    for (const auto& b : boxes) {
        if (std::abs(b.t_sec - t_sec) > hold) {
            continue;
        }
        out.push_back(
            to_detection(b, kind_or_default(quality, b.kind), width, height));
    }
    return out;
}

std::vector<Detection> RoiMapBuilder::detections_from(
    const std::vector<Box>& boxes, const Quality& quality, int width,
    int height) const
{
    std::vector<Detection> out;
    out.reserve(boxes.size());
    for (const auto& b : boxes) {
        out.push_back(
            to_detection(b, kind_or_default(quality, b.kind), width, height));
    }
    return out;
}

} // namespace roi
