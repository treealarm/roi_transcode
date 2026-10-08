#include "ta_roienc/memory.h"

namespace ta_roienc {

const char* memory_type_name(MemoryType t)
{
    switch (t) {
    case MemoryType::CPU:          return "CPU";
    case MemoryType::AVFRAME:      return "AVFRAME";
    case MemoryType::VAAPI:        return "VAAPI";
    case MemoryType::ONEVPL:       return "ONEVPL";
    }
    return "?";
}

} // namespace ta_roienc
