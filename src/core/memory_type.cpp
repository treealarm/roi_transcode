#include "sve/memory.h"

namespace sve {

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

} // namespace sve
