#pragma once

#include "roi/roi_types.h"

#include <atomic>
#include <string>

enum class EncoderKind {
    Auto,
    Qsv,
    Vaapi,
    Nvenc,
    X265,
};

struct TranscodeOptions {
    std::string input_path;
    std::string output_path;
    RoiDocument roi;
    EncoderKind encoder = EncoderKind::Auto;
    int crf = 28;
    int gop = 0; // 0 = 2 seconds
    std::string preset = "medium";
    // Passed to the muxer as "movflags"; empty leaves its default. ta_vms
    // archive clips are fragmented mp4, exports use faststart.
    std::string movflags;
};

class Transcoder {
public:
    // If cancel becomes true mid-run, throws std::runtime_error("cancelled").
    void run(const TranscodeOptions& opt, std::atomic_bool* cancel = nullptr);
};
