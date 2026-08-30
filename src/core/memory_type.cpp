#include "sve/memory.h"

namespace sve {

const char* memory_type_name(MemoryType t)
{
    switch (t) {
    case MemoryType::CPU:          return "CPU";
    case MemoryType::AVFRAME:      return "AVFRAME";
    case MemoryType::CUDA:         return "CUDA";
    case MemoryType::VAAPI:        return "VAAPI";
    case MemoryType::ONEVPL:       return "ONEVPL";
    case MemoryType::ROCKCHIP_MPP: return "ROCKCHIP_MPP";
    case MemoryType::DMABUF:       return "DMABUF";
    }
    return "?";
}

} // namespace sve
