#pragma once

// Preset in each backend's own vocabulary. Internal: a caller says how much effort they
// are buying (smartvideo::Preset) and never sees these spellings.

#include "smartvideo/types.h"

namespace smartvideo {

const char* libx26x_preset(Preset p);
const char* qsv_preset(Preset p);
const char* nvenc_preset(Preset p);

} // namespace smartvideo
