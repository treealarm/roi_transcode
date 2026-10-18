#include "ta_roienc/memory_kind.h"

namespace ta_roienc {

const char* memory_kind_name(MemoryKind t)
{
    switch (t) {
    case MemoryKind::CPU:          return "CPU";
    case MemoryKind::AVFRAME:      return "AVFRAME";
    case MemoryKind::VAAPI:        return "VAAPI";
    case MemoryKind::ONEVPL:       return "ONEVPL";
    }
    return "?";
}

} // namespace ta_roienc
