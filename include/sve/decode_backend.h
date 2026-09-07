#pragma once

// Which silicon decodes, and in what order to try -- the decode side's answer to
// sve/backend_selection.h.
//
// A separate enumeration from Backend, deliberately. The hardware entries line up one for
// one; the software ones do not. Backend::X264 names libx264, which encodes and cannot
// decode at all -- decoding on the CPU is libavcodec's own h264/hevc decoder, which is not
// a backend in the encoder's sense and has no entry over there. Folding the two lists
// would give one name two meanings depending on which direction the pixels were going,
// which is the mistake §5.4 describes in the encoder's own vocabulary.
//
// The ordering rule is borrowed intact, because the reason for it is the same: a machine
// with integrated graphics and a discrete card has no library-knowable "right" choice, so
// the list has no default and whoever deploys the system says what to try.

#include <string>
#include <string_view>
#include <vector>

namespace sve {

enum class DecodeBackend {
    /// Tries whatever DecodeSelection::preferred lists, in that order.
    Auto,
    /// VA-API, through the hardware accelerator on libavcodec's ordinary decoder.
    Vaapi,
    /// Named after the SDK rather than after Intel's marketing word for it, following
    /// §8.2 -- Quick Sync is what the hardware is called, oneVPL is what we talk to. The
    /// same silicon Vaapi reaches, through the other API.
    IntelQsv,
    /// NVDEC. Named for what decodes, not for NVENC, which is the other half of the chip.
    NvidiaNvdec,
    /// libavcodec's own decoder. An ordinary entry in the list rather than a separate
    /// "allow software fallback" switch: leaving it out is how a caller says "hardware or
    /// nothing", and putting it last is how they say "hardware if you have it".
    Cpu,
};

/// The backend's name for a log line, a report or a command line.
std::string_view decode_backend_name(DecodeBackend) noexcept;

struct DecodeSelection {
    /// Tried strictly in this order, and empty by default: an empty list is
    /// InvalidArgument rather than a silent choice made on the caller's behalf.
    std::vector<DecodeBackend> preferred;
};

/// Every name decode_selection_from_name accepts, joined by '|' in the order it lists
/// them. It exists so that an application's help text and this parser's refusal cannot
/// drift apart from what the parser does -- the encoder side learned that the hard way,
/// with the list written out by hand in three places and all three wrong.
std::string decode_selection_names();

/// One of the names decode_selection_names() lists: a backend's own name, or "auto" for
/// the order measured on this project's hardware. Unknown text throws
/// std::invalid_argument rather than defaulting, because a misspelled backend quietly
/// becoming a different one is exactly what naming it was meant to prevent.
DecodeSelection decode_selection_from_name(std::string_view name);

} // namespace sve
