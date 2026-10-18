// The names a backend answers to, and the preference order each name stands for.
//
// One table, and everything that spells a backend out loud reads it: the parser, its own
// refusal message, and any help text that asks engine_choice_names() what the parser
// accepts, so none of them can drift from what the parser does.

#include "ta_roienc/engine_order.h"

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ta_roienc {

const char* engine_name(Engine e)
{
    switch (e) {
    case Engine::Auto:  return "auto";
    case Engine::IntelOneVpl:   return "qsv";
    case Engine::Vaapi: return "vaapi";
    case Engine::X264:  return "x264";
    case Engine::X265:  return "x265";
    case Engine::X264Direct: return "x264direct";
    case Engine::X265Direct: return "x265direct";
    case Engine::IntelOneVplDirect: return "onevpldirect";
    }
    return "?";
}

namespace {

/// The library ships no default order, so the application supplies one. This is the order
/// measured on the hardware this project runs on.
///
/// The direct backends lead it. Through libavcodec iHD takes four ROI rectangles on H.264
/// and eight on H.265, so of six well-separated regions two were dropped into the crushed
/// background (SSIM spread 0.01081), while the native path paints mfxExtMBQP per 16x16
/// block and serves all of them alike (0.000144). With a target bitrate the gap is wider
/// still: the native backend opens the hardware's own VBR and holds P95 at 110% of target,
/// where the outer BitAllowance loop the libavcodec path falls back to measured 305% and
/// did not vary with the target at all.
///
/// IntelOneVpl and Vaapi stay behind it rather than being dropped: they reach the same
/// integrated GPU by another road, so a machine where the oneVPL dispatcher finds no runtime
/// still encodes on hardware. That is a fallback and not a substitution: the chain logs
/// which backend declined and why, so the weaker path can be seen rather than guessed at.
///
/// The software encoders are last because they always work, and both are listed because a
/// backend that cannot produce the requested codec is skipped here with its reason -- so
/// H.264 takes X264Direct and H.265 takes X265Direct without the caller choosing.
constexpr Engine kAutoOrder[] = {
    Engine::IntelOneVplDirect, Engine::IntelOneVpl, Engine::Vaapi,
    Engine::X264Direct, Engine::X265Direct};

/// "cpu" is Auto over exactly the two software backends: whichever of the two cannot
/// produce the codec is skipped with its reason logged.
constexpr Engine kCpuOrder[] = {Engine::X264, Engine::X265};

struct NamedEngine {
    std::string_view name;
    Engine kind;
    /// The preference order the name stands for. Empty for the names that mean one backend
    /// outright -- there is nothing to choose between, and Auto is not what was asked for.
    std::span<const Engine> order;
};

/// Every spelling accepted, in the order the refusal message lists them. One table rather
/// than a chain of comparisons beside a hand-written list, because the two drifted: the
/// message named neither the direct backends nor "qsvdirect", and a caller who typed one
/// correctly was told it did not exist.
constexpr NamedEngine kNamedEngines[] = {
    {"auto", Engine::Auto, kAutoOrder},
    {"cpu", Engine::Auto, kCpuOrder},
    {"qsv", Engine::IntelOneVpl, {}},
    {"vaapi", Engine::Vaapi, {}},
    {"x264", Engine::X264, {}},
    {"x265", Engine::X265, {}},
    {"x264direct", Engine::X264Direct, {}},
    {"x265direct", Engine::X265Direct, {}},
    {"onevpldirect", Engine::IntelOneVplDirect, {}},
    {"qsvdirect", Engine::IntelOneVplDirect, {}},
};

} // namespace

std::string engine_choice_names()
{
    std::string out;
    for (const NamedEngine& known : kNamedEngines) {
        if (!out.empty()) {
            out += '|';
        }
        out.append(known.name);
    }
    return out;
}

EngineChoice engine_choice_from_name(const char* name)
{
    const std::string_view s = name != nullptr ? name : "";
    for (const NamedEngine& known : kNamedEngines) {
        if (s != known.name) {
            continue;
        }
        EngineChoice choice;
        choice.kind = known.kind;
        choice.selection.preferred.assign(known.order.begin(), known.order.end());
        return choice;
    }
    throw std::invalid_argument(
        "unknown encoder '" + std::string(s) + "' (" + engine_choice_names() + ")");
}

} // namespace ta_roienc
