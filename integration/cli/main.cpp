#include "sve/sve.h"
#include "roi_json.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void usage()
{
    std::fprintf(stderr,
        "Usage: roi-transcode --in CLIP --roi roi.json --out OUT.mp4\n"
        "         [--encoder auto|qsv|vaapi|nvenc|x264|x265|cpu] [--codec h264|h265|source]\n"
        "         [--crf 28] [--gop N] [--preset balanced]\n"
        "         [--target-bitrate BPS] [--bitrate-overshoot F]\n"
        "\n"
        "ROI transcoder, H.264 by default. Target is Intel Quick Sync.\n"
        "--codec source keeps whatever the input already is, which is what an\n"
        "archive needs: one HLS playlist stitches many clips and browsers handle a\n"
        "codec change mid-playlist badly.\n"
        "--target-bitrate: CPU H.264 opens CBR+filler (ROI redistributes coded\n"
        "quality; mux bitrate matches). CPU H.265 and HW stay on CQP/CRF + outer\n"
        "BitrateBudget loop.\n"
        "--bitrate-overshoot is the allowed average overshoot\n"
        "fraction (default 0.1 when a target is set).\n"
        "QSV maps AVRegionOfInterest to mfxExtEncoderROI (CQP + low_power).\n"
        "Linux VAAPI is the same iHD EncSliceLP path if hevc_qsv fails.\n"
        "libx265 is the CPU fallback (needs aq-mode).\n");
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
    sve::Settings settings;
    // The library ships no default backend order since 4.1 (§8.4), so this application
    // states its own -- the same one it used when the default lived in the library.
    {
        const sve::EncoderChoice choice = sve::encoder_choice_from_name("auto");
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
                const sve::EncoderChoice choice =
                    sve::encoder_choice_from_name(need("--encoder"));
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
        settings.quality = roi_doc.quality;
        sve::transcode_file(input_path, output_path, roi_doc.boxes, settings);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
