#pragma once

// Preset in each backend's own vocabulary. Internal: a caller says how much effort they
// are buying (ta_roienc::Preset) and never sees these spellings.

#include "ta_roienc/types.h"

namespace ta_roienc {

const char* libx26x_preset(Preset p);
const char* qsv_preset(Preset p);

} // namespace ta_roienc
