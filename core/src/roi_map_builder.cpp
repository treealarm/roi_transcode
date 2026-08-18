#include "roi_map_builder.h"

#include <algorithm>
#include <cmath>

namespace {
int clamp_i(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

void clamp_rect(RoiRect& r, int width, int height)
{
    r.left = clamp_i(r.left, 0, width);
    r.right = clamp_i(r.right, 0, width);
    r.top = clamp_i(r.top, 0, height);
    r.bottom = clamp_i(r.bottom, 0, height);
}

bool rect_valid(const RoiRect& r)
{
    return r.right > r.left && r.bottom > r.top;
}

KindConfig kind_or_default(
    const RoiDocument& doc,
    const std::string& kind)
{
    auto it = doc.kinds.find(kind);
    if (it != doc.kinds.end()) {
        return it->second;
    }
    return KindConfig{};
}
} // namespace

std::vector<RoiRect> RoiMapBuilder::build(
    int width,
    int height,
    const std::vector<Detection>& detections,
    int background_qp_delta) const
{
    std::vector<RoiRect> out;
    if (width <= 0 || height <= 0) {
        return out;
    }

    // Lowest index wins on overlap (Intel VPL / FFmpeg QSV). Objects first.
    for (const auto& d : detections) {
        RoiRect obj{
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

        RoiRect pad{
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
        out.push_back(RoiRect{
            .top = 0,
            .bottom = height,
            .left = 0,
            .right = width,
            .qp_delta = background_qp_delta,
        });
    }

    return out;
}

namespace {
Detection to_detection(
    const RoiSample& s, const KindConfig& kind, int width, int height)
{
    Detection d;
    d.x1 = static_cast<int>(std::lround(s.bbox_x * width));
    d.y1 = static_cast<int>(std::lround(s.bbox_y * height));
    d.x2 = static_cast<int>(std::lround((s.bbox_x + s.bbox_w) * width));
    d.y2 = static_cast<int>(std::lround((s.bbox_y + s.bbox_h) * height));
    d.qp_delta = kind.qp_delta;
    d.pad_qp_delta = kind.qp_delta / 2;
    d.pad_x = static_cast<int>(std::lround(kind.pad_norm * width));
    d.pad_y = static_cast<int>(std::lround(kind.pad_norm * height));
    return d;
}
} // namespace

std::vector<Detection> RoiMapBuilder::detections_at(
    const RoiDocument& doc,
    int width,
    int height,
    double t_sec) const
{
    std::vector<Detection> out;
    const double hold = doc.hold_sec > 0 ? doc.hold_sec : 0.25;

    for (const auto& s : doc.samples) {
        if (std::abs(s.t - t_sec) > hold) {
            continue;
        }
        out.push_back(to_detection(s, kind_or_default(doc, s.kind), width, height));
    }
    return out;
}

std::vector<Detection> RoiMapBuilder::detections_from(
    const std::vector<RoiSample>& samples,
    const std::unordered_map<std::string, KindConfig>& kinds,
    int width,
    int height) const
{
    std::vector<Detection> out;
    out.reserve(samples.size());
    for (const auto& s : samples) {
        auto it = kinds.find(s.kind);
        const KindConfig kind = it != kinds.end() ? it->second : KindConfig{};
        out.push_back(to_detection(s, kind, width, height));
    }
    return out;
}
