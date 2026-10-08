#pragma once

// Throwing the library's internal failure, and nothing else.
//
// Separate from av_raii.h's check() so that a file that only wants to report an error does
// not get an FFmpeg header on its include path. check() does need libavutil -- it turns an
// AVERROR into a message through av_strerror -- but fail() does not, and roi_encoder.cpp is
// on the FFmpeg-free core side and calls it.
//
// av_raii.h includes this header, so the callers that reach fail() through it see exactly
// one definition.

#include <stdexcept>
#include <string>

namespace roiav {

[[noreturn]] inline void fail(const std::string& msg)
{
    throw std::runtime_error(msg);
}

} // namespace roiav
