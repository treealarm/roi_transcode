// Effort names, and the per-backend vocabularies they map onto.
//
// The mapping is a table rather than a formula because the lists are not comparable: QSV's
// "veryfast" is not libx264's "veryfast". What is comparable is the caller's intent, which
// is what Effort names.

#include "effort.h"

#include <cstring>

namespace ta_roienc {

const char* effort_name(Effort p)
{
    switch (p) {
    case Effort::Fastest:  return "fastest";
    case Effort::Fast:     return "fast";
    case Effort::Balanced: return "balanced";
    case Effort::Slow:     return "slow";
    case Effort::Slowest:  return "slowest";
    }
    return "balanced";
}

Effort effort_from_name(const char* name)
{
    if (!name) {
        return Effort::Balanced;
    }
    // Our five names and nothing else. Accepting libx264's spellings as well was tempting
    // -- it costs one table -- but it would have meant "veryfast" quietly landing on a
    // different amount of work depending on which backend opened, which is the confusion
    // the enum exists to end. The scripts and the wire format say the new names now.
    struct Slot {
        const char* text;
        Effort preset;
    };
    static const Slot table[] = {
        {"fastest", Effort::Fastest},
        {"fast", Effort::Fast},
        {"balanced", Effort::Balanced},
        {"slow", Effort::Slow},
        {"slowest", Effort::Slowest},
    };
    for (const Slot& e : table) {
        if (std::strcmp(e.text, name) == 0) {
            return e.preset;
        }
    }
    return Effort::Balanced;
}

const char* libx26x_preset(Effort p)
{
    switch (p) {
    case Effort::Fastest:  return "ultrafast";
    case Effort::Fast:     return "veryfast";
    case Effort::Balanced: return "medium";
    case Effort::Slow:     return "slow";
    case Effort::Slowest:  return "veryslow";
    }
    return "medium";
}

const char* qsv_preset(Effort p)
{
    // No "ultrafast" here: the fastest QSV names is "veryfast", and asking for one that
    // does not exist makes the encoder refuse to open rather than pick something near.
    switch (p) {
    case Effort::Fastest:  return "veryfast";
    case Effort::Fast:     return "faster";
    case Effort::Balanced: return "medium";
    case Effort::Slow:     return "slow";
    case Effort::Slowest:  return "veryslow";
    }
    return "medium";
}

const char* nvenc_preset(Effort p)
{
    // P1 to P7, NVENC's own scale from fastest to best. It also accepts names like "fast" and
    // "hq", but those are mapped onto the P scale underneath, so the scale is what to ask for.
    switch (p) {
    case Effort::Fastest:  return "p1";
    case Effort::Fast:     return "p2";
    case Effort::Balanced: return "p4";
    case Effort::Slow:     return "p6";
    case Effort::Slowest:  return "p7";
    }
    return "p4";
}

} // namespace ta_roienc
