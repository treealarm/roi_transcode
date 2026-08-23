#include "smartvideo/smartvideo.h"
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
        "         [--encoder auto|qsv|vaapi|nvenc|cpu] [--codec h265|h264]\n"
        "         [--crf 28] [--gop N] [--preset balanced]\n"
        "\n"
        "ROI transcoder, HEVC by default. Target is Intel Quick Sync.\n"
        "--codec h264 keeps a ta_vms archive homogeneous, which matters because\n"
        "one HLS playlist stitches many clips and browsers handle a codec change\n"
        "mid-playlist badly.\n"
        "QSV maps AVRegionOfInterest to mfxExtEncoderROI (CQP + low_power).\n"
        "Linux VAAPI is the same iHD EncSliceLP path if hevc_qsv fails.\n"
        "libx265 is the CPU fallback (needs aq-mode).\n");
}

smartvideo::Encoder parse_encoder(const char* s)
{
    if (std::strcmp(s, "auto") == 0) return smartvideo::Encoder::Auto;
    if (std::strcmp(s, "qsv") == 0) return smartvideo::Encoder::Qsv;
    if (std::strcmp(s, "vaapi") == 0) return smartvideo::Encoder::Vaapi;
    if (std::strcmp(s, "nvenc") == 0) return smartvideo::Encoder::Nvenc;
    if (std::strcmp(s, "cpu") == 0) return smartvideo::Encoder::Cpu;
    // The old spelling, kept so existing scripts keep working.
    if (std::strcmp(s, "x265") == 0) return smartvideo::Encoder::Cpu;
    throw std::runtime_error("unknown --encoder (auto|qsv|vaapi|nvenc|cpu)");
}
smartvideo::Codec parse_codec(const char* s)
{
    if (std::strcmp(s, "h264") == 0) return smartvideo::Codec::H264;
    if (std::strcmp(s, "h265") == 0 || std::strcmp(s, "hevc") == 0)
        return smartvideo::Codec::H265;
    throw std::runtime_error("unknown --codec (h264|h265)");
}
} // namespace

int main(int argc, char** argv)
{
    std::string input_path;
    std::string output_path;
    RoiJson roi_doc;
    bool have_roi = false;
    smartvideo::Settings settings;

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
                settings.encoder = parse_encoder(need("--encoder"));
            } else if (std::strcmp(a, "--codec") == 0) {
                settings.codec = parse_codec(need("--codec"));
            } else if (std::strcmp(a, "--crf") == 0) {
                settings.crf = std::stoi(need("--crf"));
            } else if (std::strcmp(a, "--gop") == 0) {
                settings.gop = std::stoi(need("--gop"));
            } else if (std::strcmp(a, "--preset") == 0) {
                settings.preset = smartvideo::preset_from_name(need("--preset"));
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
        smartvideo::transcode_file(input_path, output_path, roi_doc.boxes, settings);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
