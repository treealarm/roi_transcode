#include "roi_json.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

fs::path g_dir;

std::string write_json(const char* name, const std::string& body)
{
    const fs::path p = g_dir / name;
    std::ofstream(p) << body;
    return p.string();
}

bool throws(const std::string& path)
{
    try {
        load_roi_json(path);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

} // namespace

int main()
{
    g_dir = fs::temp_directory_path() / "roi_json_test";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    // Defaults when only the required samples[] is present.
    {
        const auto doc = load_roi_json(write_json("min.json", R"({"samples":[]})"));
        CHECK(doc.quality.background_qp_delta == 6);
        CHECK(doc.quality.hold_sec == 0.25);
        CHECK(doc.quality.kinds.empty());
        CHECK(doc.boxes.empty());
    }

    // background_qp_delta 0 means "leave the background alone" and must survive
    // as 0 rather than falling back to the default. This is the whole reason the
    // field is optional on the wire.
    {
        const auto doc = load_roi_json(
            write_json("bg0.json", R"({"background_qp_delta":0,"samples":[]})"));
        CHECK(doc.quality.background_qp_delta == 0);
    }

    // hold_sec 0 likewise stays 0 at parse time; the builder is what treats a
    // non-positive hold as its own default.
    {
        const auto doc = load_roi_json(
            write_json("hold0.json", R"({"hold_sec":0,"samples":[]})"));
        CHECK(doc.quality.hold_sec == 0.0);
    }

    // Full document.
    {
        const auto doc = load_roi_json(write_json("full.json", R"({
          "background_qp_delta": 20,
          "hold_sec": 1.5,
          "kinds": { "plate": {"qp_delta": -8, "pad_norm": 0.01},
                     "face":  {"qp_delta": -4} },
          "samples": [
            {"t": 1.25, "kind": "plate",
             "bbox_x": 0.35, "bbox_y": 0.4, "bbox_w": 0.3, "bbox_h": 0.2},
            {"t": 2.0, "kind": "face"}
          ]
        })"));
        CHECK(doc.quality.background_qp_delta == 20);
        CHECK(doc.quality.hold_sec == 1.5);
        CHECK(doc.quality.kinds.size() == 2);
        CHECK(doc.quality.kinds.at("plate").qp_delta == -8);
        CHECK(doc.quality.kinds.at("plate").pad == 0.01);
        // A kind that omits pad_norm takes the default, not zero.
        CHECK(doc.quality.kinds.at("face").qp_delta == -4);
        CHECK(doc.quality.kinds.at("face").pad == 0.02);
        CHECK(doc.boxes.size() == 2);
        CHECK(doc.boxes[0].kind == "plate");
        CHECK(doc.boxes[0].t_sec == 1.25);
        CHECK(doc.boxes[0].width == 0.3f);
        // A sample with no geometry parses to an empty box rather than failing.
        CHECK(doc.boxes[1].kind == "face");
        CHECK(doc.boxes[1].width == 0.0f);
    }

    // Nested analytics / quality-script shape: one sample per frame, boxes[] inside.
    {
        const auto doc = load_roi_json(write_json("nested.json", R"({
          "background_qp_delta": 4,
          "pad": 0.03,
          "kinds": { "license_plate": {"qp_delta": -12} },
          "samples": [
            {"t_sec": 1.5, "boxes": [
              {"kind": "vehicle", "x": 0.1, "y": 0.2, "width": 0.3, "height": 0.4,
               "track_id": 7, "confidence": 0.9},
              {"kind": "license_plate", "x": 0.2, "y": 0.3, "w": 0.05, "h": 0.02}
            ]},
            {"t_sec": 2.0, "boxes": []}
          ]
        })"));
        CHECK(doc.quality.background_qp_delta == 4);
        CHECK(doc.quality.pad == 0.03);
        CHECK(doc.quality.kinds.at("license_plate").qp_delta == -12);
        CHECK(doc.boxes.size() == 2);
        CHECK(doc.boxes[0].kind == "vehicle");
        CHECK(doc.boxes[0].t_sec == 1.5);
        CHECK(doc.boxes[0].x == 0.1f);
        CHECK(doc.boxes[0].width == 0.3f);
        CHECK(doc.boxes[0].track_id == 7);
        CHECK(doc.boxes[1].kind == "license_plate");
        CHECK(doc.boxes[1].width == 0.05f);
        CHECK(doc.boxes[1].height == 0.02f);
    }

    // samples[] is required: without it the caller asked for nothing, and
    // silently transcoding the whole clip at background quality would be worse
    // than saying so.
    CHECK(throws(write_json("nosamples.json", R"({"background_qp_delta":6})")));
    CHECK(throws(write_json("samples_not_array.json", R"({"samples":{}})")));
    CHECK(throws(write_json("broken.json", "{not json")));
    CHECK(throws((g_dir / "missing.json").string()));

    fs::remove_all(g_dir);
    std::puts("roi_json_test ok");
    return 0;
}
