#pragma once

// Which backends may be tried, and in what order.
//
// The list has no default, and that is the whole point of the type. A default order would
// say, for instance, that a machine with both integrated graphics and a discrete card must
// prefer the integrated one -- usually the weaker choice. A library cannot know that
// ordering; whoever deploys the system can, and has to say so.
//
// The software encoders are ordinary entries in the same list. There is no separate
// "allow software fallback" switch, because a switch and a list disagreeing about the same
// question is one place too many to look: leaving X264 and X265 out of `preferred` is how a
// caller says "hardware or nothing", and putting them last is how they say "hardware if
// you have it".

#include <string>
#include <vector>

namespace ta_roienc {

// Which backend does the encoding.
//
// For the hardware entries this says nothing about the codec -- the same silicon encodes
// both. The software entries are the exception, and deliberately so: libx264 and libx265
// are separate libraries with separate APIs, so they are separate backends rather than one
// that picks a library from the codec.
//
// The practical consequence is in selection. Naming X264 and asking for H.265 is not a
// request the library can quietly satisfy: under Auto that backend is skipped with the
// reason logged, and named outright it fails with Unsupported.
enum class Engine {
    // Tries whatever `EngineOrder::preferred` lists, in that order. Which one opened
    // is reported back, because on this hardware the difference is worth knowing.
    Auto,
    /// Named after the SDK rather than after Intel's marketing word for it. Quick Sync is
    /// what the hardware is called; oneVPL is what we talk to.
    IntelOneVpl,
    /// Reaches the same integrated GPU when the oneVPL dispatcher finds no runtime.
    Vaapi,
    X264,
    X265,
    /// libx264 through its own API rather than through libavcodec.
    X264Direct,
    /// libx265 through its own API rather than through libavcodec.
    X265Direct,
    /// Intel oneVPL through its own API. IntelOneVpl is the same hardware through
    /// libavcodec.
    IntelOneVplDirect,
    /// NVIDIA NVENC through libavcodec, on a CUDA device. It takes no regions: libavcodec's
    /// nvenc reads no AVRegionOfInterest, so the whole picture gets the same quantiser.
    Nvenc,
};


/// The backend's name for a log line, a report or a command line.
const char* engine_name(Engine e);

struct EngineOrder {
    /// Tried strictly in this order, and empty by default: under Engine::Auto an empty
    /// list is InvalidArgument rather than a silent choice made on the caller's behalf.
    ///
    /// A backend that cannot produce the requested codec is skipped here with its reason
    /// logged, so X264 in a list used to encode H.265 costs nothing and hides nothing.
    std::vector<Engine> preferred;

    /// Whether a backend absent from `preferred` may be used. False by default: a list
    /// that can be silently departed from is not a list.
    bool allow_unlisted = false;
};

/// A backend named on a command line or in a config file, together with the preference
/// order that name implies -- an application-side convenience.
///
/// It exists because two spellings do not name a single backend: "auto" needs an order,
/// and "cpu" means both software encoders.
struct EngineChoice {
    Engine kind = Engine::Auto;
    EngineOrder selection;
};

/// Every name engine_choice_from_name accepts, joined by '|' in the order it lists them.
///
/// It exists so that an application's help text and this parser's own refusal cannot drift
/// apart from what the parser does: one table feeds all of them.
std::string engine_choice_names();

/// One of the names engine_choice_names() lists: a backend's own name, "cpu" for the two
/// software encoders together, or "auto" for the order measured on this project's hardware.
/// Unknown text throws std::invalid_argument rather than defaulting: a misspelled backend
/// quietly becoming a different one is exactly the substitution this type exists to
/// prevent.
EngineChoice engine_choice_from_name(const char* name);

} // namespace ta_roienc
