#pragma once

// Effort in each backend's own vocabulary. Internal: a caller says how much effort they
// are buying (ta_roienc::Effort) and never sees these spellings.

#include "ta_roienc/basics.h"

namespace ta_roienc {

const char* libx26x_preset(Effort p);
const char* qsv_preset(Effort p);

} // namespace ta_roienc
