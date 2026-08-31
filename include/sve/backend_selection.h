#pragma once

// Which backends may be tried, and in what order -- TZ v4.1 §8.4.
//
// The list has no default, and that is the whole point of the type. v3.4 shipped one, and
// taken as a rule it said that a machine with both integrated graphics and a discrete card
// must prefer the integrated one -- usually the weaker choice. A library cannot know that
// ordering; whoever deploys the system can, and now has to say so.
//
// The software encoders are ordinary entries in the same list. There is no separate
// "allow software fallback" switch any more, because a switch and a list disagreeing about
// the same question is one place too many to look: leaving X264 and X265 out of `preferred`
// is how a caller says "hardware or nothing", and putting them last is how they say
// "hardware if you have it".

#include <vector>

namespace sve {

// Which backend does the encoding.
//
// For the hardware entries this says nothing about the codec -- the same silicon encodes
// both, and conflating the two is what made the old X265 name wrong the moment H.264
// output existed. The two software entries are the exception, and deliberately so: libx264
// and libx265 are separate libraries with separate APIs, so they are separate backends
// rather than one that picks a library from the codec (TZ v4.1 §5.4).
//
// The practical consequence is in selection. Naming X264 and asking for H.265 is not a
// request the library can quietly satisfy: under Auto that backend is skipped with the
// reason logged, and named outright it fails with Unsupported (§8.4).
enum class Backend {
    // Tries whatever `BackendSelection::preferred` lists, in that order. Which one opened
    // is reported back, because on this hardware the difference is worth knowing.
    Auto,
    /// Named after the SDK rather than after Intel's marketing word for it, following
    /// §8.2. Quick Sync is what the hardware is called; oneVPL is what we talk to.
    IntelOneVpl,
    /// Not in §8.2's enumeration at all. Kept as this library's own: it reaches the same
    /// integrated GPU when the oneVPL dispatcher finds no runtime, and it is what proved
    /// the four-region limit on Intel H.264 (see the measurements in STATUS.md). Whether
    /// it survives is question 10 to the customer, not something to decide by deleting it.
    Vaapi,
    NvidiaNvenc,
    X264,
    X265,
    /// libx264 through its own API rather than through libavcodec -- TZ v4.1 §5.4.
    /// Alongside X264 rather than instead of it, for as long as the two need comparing:
    /// integration/scripts/backend-compare.sh is what says whether the direct path keeps
    /// the zone as well, and it cannot say that if only one of them exists.
    X264Direct,
};


/// The backend's name for a log line, a report or a command line.
const char* backend_name(Backend e);

struct BackendSelection {
    /// Tried strictly in this order, and empty by default: under Backend::Auto an empty
    /// list is InvalidArgument rather than a silent choice made on the caller's behalf.
    ///
    /// A backend that cannot produce the requested codec is skipped here with its reason
    /// logged -- X264 in a list used to encode H.265 costs nothing and hides nothing.
    std::vector<Backend> preferred;

    /// Whether a backend absent from `preferred` may be used. False by default: a list
    /// that can be silently departed from is not a list.
    bool allow_unlisted = false;
};

/// A backend named on a command line or in a config file, together with the preference
/// order that name implies -- an application-side convenience, not part of §8.4.
///
/// It exists because two spellings do not name a single backend. "auto" needs an order,
/// and since 4.1 the library supplies none. "cpu" named one backend until 4.1 split it in
/// two, and every existing script and config still says it.
struct BackendChoice {
    Backend kind = Backend::Auto;
    BackendSelection selection;
};

/// "auto", "qsv", "vaapi", "nvenc", "x264", "x265", or "cpu" for the two software encoders
/// together. Unknown text throws std::invalid_argument rather than defaulting: a misspelled
/// backend quietly becoming a different one is the substitution §8.4 exists to prevent.
BackendChoice backend_choice_from_name(const char* name);

} // namespace sve
