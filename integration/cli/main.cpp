#include "sve/backend_loader.h"
#include "sve/backend_registry.h"
#include "sve/sve.h"
#include "roi_json.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {
void usage()
{
    // The backend names come from the parser rather than from a list written here. They
    // were written here once, and went stale the day §5.4 and §5.2 added the direct
    // backends: --encoder x264direct worked and the usage text denied it existed.
    std::fprintf(stderr,
        "Usage: roi-transcode --in CLIP --roi roi.json --out OUT.mp4\n"
        "         [--encoder %s]\n"
        "         [--codec h264|h265|source] [--crf 28] [--gop N] [--preset balanced]\n"
        "         [--target-bitrate BPS] [--bitrate-overshoot F] [--sidecar]\n"
        "         [--plugin-dir DIR]...\n"
        "\n"
        "ROI transcoder, H.264 by default.\n"
        "--codec source keeps whatever the input already is, which is what an\n"
        "archive needs: one HLS playlist stitches many clips and browsers handle a\n"
        "codec change mid-playlist badly.\n"
        "\n"
        "The names ending in 'direct' reach the encoder's own API instead of going\n"
        "through libavcodec (TZ §5.2, §5.4), and that is where per-block quality\n"
        "lives: x264direct and x265direct carry per-block quantiser offsets,\n"
        "onevpldirect carries an mfxExtMBQP map. qsv and vaapi reach the same Intel\n"
        "GPU through libavcodec, where a region is a rectangle and nothing finer.\n"
        "'cpu' is x264 and x265 together, whichever can produce the codec; 'auto'\n"
        "is the order measured on this project's hardware.\n"
        "\n"
        "--target-bitrate: x264direct and x265direct honour it themselves with\n"
        "ABR+VBV and keep the block map, because offsets compose with rate control.\n"
        "onevpldirect switches to VBR and trades the map for rectangles, because\n"
        "mfxExtMBQP carries absolute quantisers and needs CQP. The libavcodec x264\n"
        "path opens CBR+filler. The rest ride the outer BitrateBudget loop.\n"
        "--bitrate-overshoot is the allowed average overshoot fraction (default 0.1\n"
        "when a target is set).\n"
        "\n"
        "--plugin-dir names a directory of backend plugins to load before encoding, and\n"
        "may be repeated. Whatever this build linked is registered already; §26.4's own\n"
        "directories -- beside the program, then the configured system one -- are searched\n"
        "after any given here. A plugin that is refused says so on stderr; a directory with\n"
        "no plugins in it is not an error.\n"
        "\n"
        "--sidecar writes <output_basename>.svemeta.ndjson beside the output (TZ §19.3):\n"
        "one JSON line per emitted packet, carrying the objects that frame was submitted\n"
        "with. Independent of SEI -- it is what carries the metadata when the bitstream\n"
        "cannot, and its lines follow emission order including B-frame reordering.\n",
        sve::backend_choice_names().c_str());
}

sve::Codec parse_codec(const char* s)
{
    if (std::strcmp(s, "source") == 0) return sve::Codec::SameAsSource;
    if (std::strcmp(s, "h264") == 0) return sve::Codec::H264;
    if (std::strcmp(s, "h265") == 0 || std::strcmp(s, "hevc") == 0)
        return sve::Codec::H265;
    throw std::runtime_error("unknown --codec (h264|h265|source)");
}
} // namespace

int main(int argc, char** argv)
{
    std::string input_path;
    std::string output_path;
    RoiJson roi_doc;
    bool have_roi = false;
    std::vector<std::filesystem::path> plugin_dirs;
    sve::Settings settings;
    // The library ships no default backend order since 4.1 (§8.4), so this application
    // states its own -- the same one it used when the default lived in the library.
    {
        const sve::BackendChoice choice = sve::backend_choice_from_name("auto");
        settings.encoder = choice.kind;
        settings.backend_selection = choice.selection;
    }

    try {
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc) {
                    throw std::runtime_error(std::string("missing value for ")
                        + name);
                }
                return argv[++i];
            };
            if (std::strcmp(a, "--in") == 0) {
                input_path = need("--in");
            } else if (std::strcmp(a, "--out") == 0) {
                output_path = need("--out");
            } else if (std::strcmp(a, "--roi") == 0) {
                roi_doc = load_roi_json(need("--roi"));
                have_roi = true;
            } else if (std::strcmp(a, "--encoder") == 0) {
                const sve::BackendChoice choice =
                    sve::backend_choice_from_name(need("--encoder"));
                settings.encoder = choice.kind;
                settings.backend_selection = choice.selection;
            } else if (std::strcmp(a, "--codec") == 0) {
                settings.codec = parse_codec(need("--codec"));
            } else if (std::strcmp(a, "--crf") == 0) {
                settings.crf = std::stoi(need("--crf"));
            } else if (std::strcmp(a, "--gop") == 0) {
                settings.gop = std::stoi(need("--gop"));
            } else if (std::strcmp(a, "--preset") == 0) {
                settings.preset = sve::preset_from_name(need("--preset"));
            } else if (std::strcmp(a, "--target-bitrate") == 0) {
                settings.rate_control.target_bitrate =
                    static_cast<uint64_t>(std::stoull(need("--target-bitrate")));
            } else if (std::strcmp(a, "--bitrate-overshoot") == 0) {
                settings.rate_control.max_average_overshoot =
                    std::stof(need("--bitrate-overshoot"));
            } else if (std::strcmp(a, "--plugin-dir") == 0) {
                plugin_dirs.emplace_back(need("--plugin-dir"));
            } else if (std::strcmp(a, "--sidecar") == 0) {
                settings.metadata.sidecar = sve::SidecarFormat::NDJSON;
            } else if (std::strcmp(a, "-h") == 0
                || std::strcmp(a, "--help") == 0) {
                usage();
                return 0;
            } else {
                throw std::runtime_error(std::string("unknown arg: ") + a);
            }
        }
        if (input_path.empty() || output_path.empty() || !have_roi) {
            usage();
            return 2;
        }
        // §26.4's dynamic half. What this build linked is registered already; this adds
        // what the deployment shipped as modules, and says out loud what it refused --
        // §50's rule is that a plugin quietly not loading is the worst of the outcomes,
        // because the encode then succeeds with the wrong backend and nothing says so.
        {
            // A directory the user named and that is not there is worth saying out loud,
            // even though the library treats an absent directory as nothing to load: the
            // library cannot tell a configured default from a typed argument, and this can.
            for (const std::filesystem::path& dir : plugin_dirs) {
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) {
                    std::fprintf(stderr, "plugin directory not found: %s\n",
                        dir.string().c_str());
                }
            }
            sve::PluginSearch search;
            search.paths = plugin_dirs;
            const sve::PluginLoadReport report =
                sve::LoadBackendPlugins(sve::default_backend_registry(), search);
            for (const std::string& line : report.rejected) {
                std::fprintf(stderr, "plugin refused: %s\n", line.c_str());
            }
            for (const sve::Backend kind : report.loaded) {
                std::fprintf(stderr, "plugin loaded: %s\n", sve::backend_name(kind));
            }
        }

        settings.quality = roi_doc.quality;
        sve::transcode_file(input_path, output_path, roi_doc.boxes, settings);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
