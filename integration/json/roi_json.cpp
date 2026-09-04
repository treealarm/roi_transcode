#include "roi_json.h"

#include <cstdio>

#include <algorithm>

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

float box_dim(const json& row, const char* a, const char* b, float fallback = 0.0f)
{
    if (row.contains(a)) {
        return row.value(a, fallback);
    }
    if (row.contains(b)) {
        return row.value(b, fallback);
    }
    return fallback;
}

double sample_time(const json& row)
{
    if (row.contains("t_sec")) {
        return row.value("t_sec", 0.0);
    }
    return row.value("t", 0.0);
}

sve::Box parse_box(const json& row, double t_sec)
{
    sve::Box b;
    b.t_sec = t_sec;
    b.kind = row.value("kind", std::string{});
    b.x = box_dim(row, "x", "bbox_x");
    b.y = box_dim(row, "y", "bbox_y");
    b.width = box_dim(row, "width", "bbox_w");
    // Analytics sidecar uses "w"/"h"; the library flat form uses bbox_w/bbox_h.
    if (b.width <= 0.0f && row.contains("w")) {
        b.width = row.value("w", 0.0f);
    }
    b.height = box_dim(row, "height", "bbox_h");
    if (b.height <= 0.0f && row.contains("h")) {
        b.height = row.value("h", 0.0f);
    }
    b.track_id = row.value("track_id", 0ULL);
    b.confidence = row.value("confidence", 0.0f);
    b.label = row.value("label", std::string{});
    return b;
}

} // namespace

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
    doc.quality.pad = root.value("pad", doc.quality.pad);
    if (root.contains("max_regions")) {
        doc.quality.max_regions = root.value("max_regions", 32);
    }
    // §15's area budget. Present so a config can move it -- the default is a quarter of
    // the frame and lives in Quality, not here.
    if (root.contains("max_protected_fraction")) {
        doc.quality.max_protected_fraction = root.value("max_protected_fraction", 0.25f);
        if (doc.quality.max_protected_fraction == 0.0f) {
            // Said out loud because the meaning of this exact value changed on 2026-09-04,
            // and a document written before that meant the opposite of what it now does.
            // A silently reinterpreted config is the shape of failure §50 forbids, and this
            // is the only place that can see the document as written.
            std::fprintf(stderr,
                "roi json: max_protected_fraction is 0, which protects nothing. Before "
                "2026-09-04 this value switched the budget off and protected everything; "
                "if that is what was meant, the value for it is now larger than 1.\n");
        }
    }

    if (root.contains("kinds") && root["kinds"].is_object()) {
        for (auto it = root["kinds"].begin(); it != root["kinds"].end(); ++it) {
            sve::KindQuality cfg;
            cfg.qp_delta = it.value().value("qp_delta", -6);
            cfg.pad = it.value().value("pad_norm", doc.quality.pad);
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
        const double t = sample_time(row);
        // Nested form from analytics-worker / quality scripts:
        //   { "t_sec": ..., "boxes": [ {kind,x,y,width,height,...}, ... ] }
        // Flat library form (sweep-padding, unit tests):
        //   { "t": ..., "kind": ..., "bbox_x": ..., ... }
        if (row.contains("boxes") && row["boxes"].is_array()) {
            for (const auto& box_row : row["boxes"]) {
                doc.boxes.push_back(parse_box(box_row, t));
            }
            continue;
        }
        doc.boxes.push_back(parse_box(row, t));
    }

    return doc;
}
