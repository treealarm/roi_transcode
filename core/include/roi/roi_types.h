#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class EncoderKind {
    Auto,
    Qsv,
    Vaapi,
    Nvenc,
    X265,
};

struct KindConfig {
    int qp_delta = -6;
    double pad_norm = 0.02;
};

struct RoiSample {
    double t = 0;
    std::string kind;
    float bbox_x = 0;
    float bbox_y = 0;
    float bbox_w = 0;
    float bbox_h = 0;
};

struct RoiDocument {
    int background_qp_delta = 6;
    double hold_sec = 0.25;
    std::unordered_map<std::string, KindConfig> kinds;
    std::vector<RoiSample> samples;
};

struct Detection {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int qp_delta = -6;
    int pad_qp_delta = -3;
    int pad_x = 0;
    int pad_y = 0;
};

struct RoiRect {
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    int qp_delta = 0;
};
