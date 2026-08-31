#pragma once

// Throwing the library's internal failure, and nothing else.
//
// This lived in av_raii.h next to check(), which is where it was first needed, and that
// put an FFmpeg header on the include path of every file that only wanted to report an
// error. check() does need libavutil -- it turns an AVERROR into a message through
// av_strerror -- but fail() never did. §5.5 draws the line between the core and anything
// that links FFmpeg, and roi_encoder.cpp is on the core side of it and calls this eight
// times.
//
// av_raii.h includes this header, so the callers that reach fail() through it are
// unchanged and there is still exactly one definition. The roiav namespace is kept for the
// same reason: renaming it would touch six files to no effect other than the name.

#include <stdexcept>
#include <string>

namespace roiav {

[[noreturn]] inline void fail(const std::string& msg)
{
    throw std::runtime_error(msg);
}

} // namespace roiav
