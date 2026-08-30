#pragma once

// roi.json -> the vocabulary roi_core takes. Deliberately outside roi_core:
// embedding the encoder should not drag a JSON library along, and only the CLI
// and the service ever read a file.

#include "sve/types.h"

#include <string>
#include <vector>

struct RoiJson {
    sve::Quality quality;
    std::vector<sve::Box> boxes;
};

RoiJson load_roi_json(const std::string& path);
