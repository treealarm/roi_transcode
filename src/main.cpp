#include "roi_json.h"
#include "transcoder.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void usage()
{
    std::fprintf(stderr,
        "Usage: roi-transcode --in CLIP --roi roi.json --out OUT.mp4\n"
        "         [--encoder auto|qsv|vaapi|nvenc|x265] [--crf 28] [--gop N] [--preset medium]\n"
        "\n"
        "HEVC ROI transcoder. Target is Intel Quick Sync.\n"
        "QSV maps AVRegionOfInterest to mfxExtEncoderROI (CQP + low_power).\n"
        "Linux VAAPI is the same iHD EncSliceLP path if hevc_qsv fails.\n"
        "libx265 is the CPU fallback (needs aq-mode).\n");
}

EncoderKind parse_encoder(const char* s)
{
    if (std::strcmp(s, "auto") == 0) {
        return EncoderKind::Auto;
    }
    if (std::strcmp(s, "qsv") == 0) {
        return EncoderKind::Qsv;
    }
    if (std::strcmp(s, "vaapi") == 0) {
        return EncoderKind::Vaapi;
    }
    if (std::strcmp(s, "nvenc") == 0) {
        return EncoderKind::Nvenc;
    }
    if (std::strcmp(s, "x265") == 0) {
        return EncoderKind::X265;
    }
    throw std::runtime_error("unknown --encoder (auto|qsv|vaapi|nvenc|x265)");
}
} // namespace

int main(int argc, char** argv)
{
    TranscodeOptions opt;
    bool have_roi = false;
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
                opt.input_path = need("--in");
            } else if (std::strcmp(a, "--out") == 0) {
                opt.output_path = need("--out");
            } else if (std::strcmp(a, "--roi") == 0) {
                opt.roi = load_roi_json(need("--roi"));
                have_roi = true;
            } else if (std::strcmp(a, "--encoder") == 0) {
                opt.encoder = parse_encoder(need("--encoder"));
            } else if (std::strcmp(a, "--crf") == 0) {
                opt.crf = std::stoi(need("--crf"));
            } else if (std::strcmp(a, "--gop") == 0) {
                opt.gop = std::stoi(need("--gop"));
            } else if (std::strcmp(a, "--preset") == 0) {
                opt.preset = need("--preset");
            } else if (std::strcmp(a, "-h") == 0
                || std::strcmp(a, "--help") == 0) {
                usage();
                return 0;
            } else {
                throw std::runtime_error(std::string("unknown arg: ") + a);
            }
        }
        if (opt.input_path.empty() || opt.output_path.empty() || !have_roi) {
            usage();
            return 2;
        }
        Transcoder{}.run(opt);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
