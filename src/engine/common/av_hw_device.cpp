#include "engine/common/av_hw_device.h"

namespace ta_roienc {

bool hw_device_type_present(AVHWDeviceType type) noexcept
{
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         t != AV_HWDEVICE_TYPE_NONE; t = av_hwdevice_iterate_types(t)) {
        if (t == type) {
            return true;
        }
    }
    return false;
}

} // namespace ta_roienc
