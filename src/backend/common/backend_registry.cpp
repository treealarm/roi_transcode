// Which backends to try, and in what order.
//
// Encoder::Auto walks the chain; a named kind returns only itself. That asymmetry is
// deliberate and is what makes "--encoder qsv" mean it: falling back to libx264 when the
// caller asked for hardware would hide the very thing they were checking.

#include "backend/common/encoder_backend.h"

namespace smartvideo {

const IEncoderBackend& onevpl_backend();
const IEncoderBackend& vaapi_backend();
const IEncoderBackend& nvenc_backend();
const IEncoderBackend& software_backend();

std::vector<const IEncoderBackend*> backend_chain(Encoder requested)
{
    switch (requested) {
    case Encoder::Auto:
        // Quick Sync first because on the hardware this runs on it is several times
        // faster; VA-API second because it reaches the same iGPU when the oneVPL
        // dispatcher finds no runtime; the CPU last because it always works.
        return {&onevpl_backend(), &vaapi_backend(), &software_backend()};
    case Encoder::Qsv:
        return {&onevpl_backend()};
    case Encoder::Vaapi:
        return {&vaapi_backend()};
    case Encoder::Nvenc:
        return {&nvenc_backend()};
    case Encoder::Cpu:
        break;
    }
    return {&software_backend()};
}

const IEncoderBackend* backend_for(Encoder kind)
{
    for (const auto* b : backend_chain(kind)) {
        if (b->kind() == kind) {
            return b;
        }
    }
    return nullptr;
}

} // namespace smartvideo
