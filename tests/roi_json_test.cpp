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
        CHECK(doc.background_qp_delta == 6);
        CHECK(doc.hold_sec == 0.25);
        CHECK(doc.kinds.empty());
        CHECK(doc.samples.empty());
    }

    // background_qp_delta 0 means "leave the background alone" and must survive
    // as 0 rather than falling back to the default. This is the whole reason the
    // field is optional on the wire.
    {
        const auto doc = load_roi_json(
            write_json("bg0.json", R"({"background_qp_delta":0,"samples":[]})"));
        CHECK(doc.background_qp_delta == 0);
    }

    // hold_sec 0 likewise stays 0 at parse time; the builder is what treats a
    // non-positive hold as its own default.
    {
        const auto doc = load_roi_json(
            write_json("hold0.json", R"({"hold_sec":0,"samples":[]})"));
        CHECK(doc.hold_sec == 0.0);
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
        CHECK(doc.background_qp_delta == 20);
        CHECK(doc.hold_sec == 1.5);
        CHECK(doc.kinds.size() == 2);
        CHECK(doc.kinds.at("plate").qp_delta == -8);
        CHECK(doc.kinds.at("plate").pad_norm == 0.01);
        // A kind that omits pad_norm takes the default, not zero.
        CHECK(doc.kinds.at("face").qp_delta == -4);
        CHECK(doc.kinds.at("face").pad_norm == 0.02);
        CHECK(doc.samples.size() == 2);
        CHECK(doc.samples[0].kind == "plate");
        CHECK(doc.samples[0].t == 1.25);
        CHECK(doc.samples[0].bbox_w == 0.3f);
        // A sample with no geometry parses to an empty box rather than failing.
        CHECK(doc.samples[1].kind == "face");
        CHECK(doc.samples[1].bbox_w == 0.0f);
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
