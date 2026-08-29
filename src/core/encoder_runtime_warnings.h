#pragma once

#include "smartvideo/capabilities.h"

#include <cstdio>

namespace smartvideo {

inline void warn_sei_unsupported_once(const bool requested, const EncoderCapabilities& caps,
    bool& warned)
{
    if (!requested || caps.sei || warned) {
        return;
    }
    std::fprintf(stderr,
        "warning: bitstream SEI requested but not supported by encoder capabilities\n");
    warned = true;
}

} // namespace smartvideo
