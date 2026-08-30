#pragma once

// Which backends may be tried, and in what order -- TZ v3.4 §8.4.
//
// The list is a default rather than a rule. §8.4 spells one out, and taken as a rule it
// would say that a machine with both integrated graphics and a discrete card must prefer
// the integrated one -- usually the weaker choice. So whoever deploys the system orders it
// for their own hardware, and this is what they order.

#include "sve/types.h"

#include <vector>

namespace sve {

struct BackendSelection {
    /// Tried strictly in this order. The default is what this library measured on the
    /// hardware it runs on: Quick Sync first because it is several times faster here,
    /// VA-API second because it reaches the same integrated GPU when the oneVPL dispatcher
    /// finds no runtime, the CPU last because it always works.
    ///
    /// NVENC is deliberately absent. The FFmpeg wrapper for it does not read region-of-
    /// interest side data at all, so an encoder chosen automatically would quietly produce
    /// a stream with no adaptive quality in it -- the one thing this library is for. Ask
    /// for it by name and you get it; nothing picks it for you.
    std::vector<Encoder> preferred{Encoder::Qsv, Encoder::Vaapi, Encoder::Cpu};

    /// Whether a backend absent from `preferred` may be used. False by default: a list
    /// that can be silently departed from is not a list.
    bool allow_unlisted = false;

    /// Whether the software encoder may be reached automatically. Setting this false is
    /// how a caller says "hardware or nothing" -- useful when falling back to the CPU
    /// would quietly cost more than failing.
    bool allow_software_fallback = true;
};

} // namespace sve
