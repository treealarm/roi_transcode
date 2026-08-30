#pragma once

// Preset in each backend's own vocabulary. Internal: a caller says how much effort they
// are buying (sve::Preset) and never sees these spellings.

#include "sve/types.h"

namespace sve {

const char* libx26x_preset(Preset p);
const char* qsv_preset(Preset p);
const char* nvenc_preset(Preset p);

} // namespace sve
