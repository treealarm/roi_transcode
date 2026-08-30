// Preset names, and the per-backend vocabularies they map onto.
//
// The mapping is a table rather than a formula because the lists are not comparable: QSV's
// "veryfast" is not libx264's "veryfast", and NVENC's p1..p7 are not a scale of the same
// thing at all. What is comparable is the caller's intent, which is what Preset names.

#include "preset.h"

#include <cstring>

namespace sve {

const char* preset_name(Preset p)
{
    switch (p) {
    case Preset::Fastest:  return "fastest";
    case Preset::Fast:     return "fast";
    case Preset::Balanced: return "balanced";
    case Preset::Slow:     return "slow";
    case Preset::Slowest:  return "slowest";
    }
    return "balanced";
}

Preset preset_from_name(const char* name)
{
    if (!name) {
        return Preset::Balanced;
    }
    // Our five names and nothing else. Accepting libx264's spellings as well was tempting
    // -- it costs one table -- but it would have meant "veryfast" quietly landing on a
    // different amount of work depending on which backend opened, which is the confusion
    // the enum exists to end. The scripts and the wire format say the new names now.
    struct Entry {
        const char* text;
        Preset preset;
    };
    static const Entry table[] = {
        {"fastest", Preset::Fastest},
        {"fast", Preset::Fast},
        {"balanced", Preset::Balanced},
        {"slow", Preset::Slow},
        {"slowest", Preset::Slowest},
    };
    for (const Entry& e : table) {
        if (std::strcmp(e.text, name) == 0) {
            return e.preset;
        }
    }
    return Preset::Balanced;
}

const char* libx26x_preset(Preset p)
{
    switch (p) {
    case Preset::Fastest:  return "ultrafast";
    case Preset::Fast:     return "veryfast";
    case Preset::Balanced: return "medium";
    case Preset::Slow:     return "slow";
    case Preset::Slowest:  return "veryslow";
    }
    return "medium";
}

const char* qsv_preset(Preset p)
{
    // No "ultrafast" here: the fastest QSV names is "veryfast", and asking for one that
    // does not exist makes the encoder refuse to open rather than pick something near.
    switch (p) {
    case Preset::Fastest:  return "veryfast";
    case Preset::Fast:     return "faster";
    case Preset::Balanced: return "medium";
    case Preset::Slow:     return "slow";
    case Preset::Slowest:  return "veryslow";
    }
    return "medium";
}

const char* nvenc_preset(Preset p)
{
    // p1 is fastest, p7 slowest. The older named presets are deprecated in NVENC and map
    // onto these anyway.
    switch (p) {
    case Preset::Fastest:  return "p1";
    case Preset::Fast:     return "p3";
    case Preset::Balanced: return "p4";
    case Preset::Slow:     return "p6";
    case Preset::Slowest:  return "p7";
    }
    return "p4";
}

} // namespace sve
