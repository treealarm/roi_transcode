#include "roi_json.h"

#include <algorithm>

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using nlohmann::json;

RoiJson load_roi_json(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open ROI JSON: " + path);
    }

    json root;
    in >> root;

    RoiJson doc;
    doc.quality.background_qp_delta = root.value("background_qp_delta", 6);
    doc.quality.hold_sec = root.value("hold_sec", 0.25);
    if (root.contains("max_regions")) {
        doc.quality.max_regions = root.value("max_regions", 32);
    }

    if (root.contains("kinds") && root["kinds"].is_object()) {
        for (auto it = root["kinds"].begin(); it != root["kinds"].end(); ++it) {
            smartvideo::KindQuality cfg;
            cfg.qp_delta = it.value().value("qp_delta", -6);
            cfg.pad = it.value().value("pad_norm", 0.02);
            doc.quality.kinds.emplace(it.key(), cfg);
        }
    }

    // A mask has no kind names, so it borrows the strongest configured kind.
    for (const auto& [name, cfg] : doc.quality.kinds) {
        (void)name;
        doc.quality.mask_qp_delta_at_255 =
            std::min(doc.quality.mask_qp_delta_at_255, cfg.qp_delta);
    }

    if (!root.contains("samples") || !root["samples"].is_array()) {
        throw std::runtime_error("ROI JSON missing samples[]");
    }

    for (const auto& row : root["samples"]) {
        smartvideo::Box b;
        b.t_sec = row.value("t", 0.0);
        b.kind = row.value("kind", std::string{});
        b.x = row.value("bbox_x", 0.0f);
        b.y = row.value("bbox_y", 0.0f);
        b.width = row.value("bbox_w", 0.0f);
        b.height = row.value("bbox_h", 0.0f);
        doc.boxes.push_back(b);
    }

    return doc;
}
