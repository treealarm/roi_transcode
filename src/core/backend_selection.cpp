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
/// This is the order measured on the hardware this project runs on: Quick Sync first
/// because it is several times faster here, VA-API second because it reaches the same
/// integrated GPU when the oneVPL dispatcher finds no runtime, the software encoders last
/// because they always work. NVENC is absent on purpose -- the FFmpeg wrapper for it does
/// not read region-of-interest side data, so a stream chosen automatically would quietly
/// carry no adaptive quality at all.
constexpr Backend kAutoOrder[] = {
    Backend::IntelOneVpl, Backend::Vaapi, Backend::X264, Backend::X265};

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
