// Which backends to try, and in what order.
//
// Encoder::Auto walks the chain; a named kind returns only itself. That asymmetry is
// deliberate and is what makes "--encoder qsv" mean it: falling back to libx264 when the
// caller asked for hardware would hide the very thing they were checking.

#include "smartvideo/encoder_backend.h"

namespace smartvideo {

const IEncoderBackend& onevpl_backend();
const IEncoderBackend& vaapi_backend();
const IEncoderBackend& nvenc_backend();
const IEncoderBackend& software_backend();

std::span<const BackendRef> backend_chain(Encoder requested)
{
    // Function-local statics: built on first use, in an order that cannot be wrong, and
    // never destroyed while a caller could still be holding the span.
    switch (requested) {
    case Encoder::Auto: {
        // Quick Sync first because on the hardware this runs on it is several times
        // faster; VA-API second because it reaches the same iGPU when the oneVPL
        // dispatcher finds no runtime; the CPU last because it always works.
        static const BackendRef chain[] = {
            onevpl_backend(), vaapi_backend(), software_backend()};
        return chain;
    }
    case Encoder::Qsv: {
        static const BackendRef chain[] = {onevpl_backend()};
        return chain;
    }
    case Encoder::Vaapi: {
        static const BackendRef chain[] = {vaapi_backend()};
        return chain;
    }
    case Encoder::Nvenc: {
        static const BackendRef chain[] = {nvenc_backend()};
        return chain;
    }
    case Encoder::Cpu:
        break;
    }
    static const BackendRef cpu[] = {software_backend()};
    return cpu;
}

} // namespace smartvideo
