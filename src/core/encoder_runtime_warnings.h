#pragma once

#include "sve/capabilities.h"

#include <cstdio>

namespace sve {

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

} // namespace sve
