// The names a backend answers to, and the preference order each name stands for.
//
// One table, and everything that spells a backend out loud reads it: the parser, its own
// refusal message, and any help text that asks backend_choice_names() what the parser
// accepts. Those were three hand-written lists until 2026-09-01, by which point all three
// had gone stale in the same way -- none of them named the direct backends §5.4 and §5.2
// added, so `--encoder x264direct` worked while every list said it did not exist.
//
// It lived in adaptive/object_geometry.cpp, which is about bounding boxes and rasterising
// masks and has nothing to do with any of this.

#include "sve/backend_selection.h"

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sve {

const char* backend_name(Backend e)
{
    switch (e) {
    case Backend::Auto:  return "auto";
    case Backend::IntelOneVpl:   return "qsv";
    case Backend::Vaapi: return "vaapi";
    case Backend::NvidiaNvenc: return "nvenc";
    case Backend::X264:  return "x264";
    case Backend::X265:  return "x265";
    case Backend::X264Direct: return "x264direct";
    case Backend::X265Direct: return "x265direct";
    case Backend::IntelOneVplDirect: return "onevpldirect";
    }
    return "?";
}

namespace {

/// The library ships no default order any more (§8.4), so the application supplies one.
/// This is the order measured on the hardware this project runs on.
///
/// The direct backends lead it, and that is a decision taken 2026-09-01 rather than a
/// preference. What a caller who says nothing gets is what this list says, and through
/// libavcodec it was measurably less than the specification asks for: iHD takes four ROI
/// rectangles on H.264 and eight on H.265, so of six well-separated regions two were
/// dropped into the crushed background (SSIM spread 0.01081), while the native path paints
/// mfxExtMBQP per 16x16 block and serves all of them alike (0.000144). With a target
/// bitrate the gap is wider still: the native backend opens the hardware's own VBR and
/// meets §16 at P95 110% of target, where the outer BitrateBudget loop the libavcodec path
/// falls back to measured 305% and did not vary with the target at all.
///
/// IntelOneVpl and Vaapi stay behind it rather than being dropped. The native backend is
/// young -- 2026-08-31 -- and they reach the same integrated GPU by an older road, so a
/// machine where the oneVPL dispatcher finds no runtime still encodes on hardware. That is
/// a fallback and not a substitution: §8.4 has the chain log which backend declined and
/// why, so the weaker path can be seen rather than guessed at.
///
/// The software encoders are last because they always work, and both are listed because a
/// backend that cannot produce the requested codec is skipped here with its reason -- so
/// H.264 takes X264Direct and H.265 takes X265Direct without the caller choosing.
///
/// NVENC is absent on purpose: the FFmpeg wrapper for it does not read region-of-interest
/// side data, so a stream chosen automatically would quietly carry no adaptive quality.
constexpr Backend kAutoOrder[] = {
    Backend::IntelOneVplDirect, Backend::IntelOneVpl, Backend::Vaapi,
    Backend::X264Direct, Backend::X265Direct};

/// "cpu" is Auto over exactly the two software backends. That is not a workaround for the
/// split but the thing itself: whichever of the two cannot produce the codec is skipped
/// with its reason logged, which is what the one combined backend used to do silently
/// inside itself.
constexpr Backend kCpuOrder[] = {Backend::X264, Backend::X265};

struct NamedBackend {
    std::string_view name;
    Backend kind;
    /// The preference order the name stands for. Empty for the names that mean one backend
    /// outright -- there is nothing to choose between, and Auto is not what was asked for.
    std::span<const Backend> order;
};

/// Every spelling accepted, in the order the refusal message lists them. One table rather
/// than a chain of comparisons beside a hand-written list, because the two drifted: the
/// message named neither the direct backends nor "qsvdirect", and a caller who typed one
/// correctly was told it did not exist.
constexpr NamedBackend kNamedBackends[] = {
    {"auto", Backend::Auto, kAutoOrder},
    {"cpu", Backend::Auto, kCpuOrder},
    {"qsv", Backend::IntelOneVpl, {}},
    {"vaapi", Backend::Vaapi, {}},
    {"nvenc", Backend::NvidiaNvenc, {}},
    {"x264", Backend::X264, {}},
    {"x265", Backend::X265, {}},
    {"x264direct", Backend::X264Direct, {}},
    {"x265direct", Backend::X265Direct, {}},
    {"onevpldirect", Backend::IntelOneVplDirect, {}},
    {"qsvdirect", Backend::IntelOneVplDirect, {}},
};

} // namespace

std::string backend_choice_names()
{
    std::string out;
    for (const NamedBackend& known : kNamedBackends) {
        if (!out.empty()) {
            out += '|';
        }
        out.append(known.name);
    }
    return out;
}

BackendChoice backend_choice_from_name(const char* name)
{
    const std::string_view s = name != nullptr ? name : "";
    for (const NamedBackend& known : kNamedBackends) {
        if (s != known.name) {
            continue;
        }
        BackendChoice choice;
        choice.kind = known.kind;
        choice.selection.preferred.assign(known.order.begin(), known.order.end());
        return choice;
    }
    throw std::invalid_argument(
        "unknown encoder '" + std::string(s) + "' (" + backend_choice_names() + ")");
}

} // namespace sve
