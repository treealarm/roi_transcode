#include "roi_json.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using nlohmann::json;

RoiDocument load_roi_json(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open ROI JSON: " + path);
    }

    json root;
    in >> root;

    RoiDocument doc;
    doc.background_qp_delta = root.value("background_qp_delta", 6);
    doc.hold_sec = root.value("hold_sec", 0.25);

    if (root.contains("kinds") && root["kinds"].is_object()) {
        for (auto it = root["kinds"].begin(); it != root["kinds"].end(); ++it) {
            KindConfig cfg;
            cfg.qp_delta = it.value().value("qp_delta", -6);
            cfg.pad_norm = it.value().value("pad_norm", 0.02);
            doc.kinds.emplace(it.key(), cfg);
        }
    }

    if (!root.contains("samples") || !root["samples"].is_array()) {
        throw std::runtime_error("ROI JSON missing samples[]");
    }

    for (const auto& row : root["samples"]) {
        RoiSample s;
        s.t = row.value("t", 0.0);
        s.kind = row.value("kind", std::string{});
        s.bbox_x = row.value("bbox_x", 0.0f);
        s.bbox_y = row.value("bbox_y", 0.0f);
        s.bbox_w = row.value("bbox_w", 0.0f);
        s.bbox_h = row.value("bbox_h", 0.0f);
        doc.samples.push_back(s);
    }

    return doc;
}
