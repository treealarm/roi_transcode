#include "ta_roienc/decode_backend.h"

#include <format>
#include <stdexcept>

namespace ta_roienc {

namespace {

/// Name, backend, and what naming it implies. One table, because the parser, the joined
/// name list and the reverse lookup are three views of the same fact and drifted apart on
/// the encoder side when they were three lists.
struct Entry {
    std::string_view name;
    DecodeBackend backend;
};

// "auto" is not here: it names an order rather than a backend, and is handled separately.
constexpr Entry k_entries[] = {
    {"vaapi", DecodeBackend::Vaapi},
    {"qsv", DecodeBackend::IntelQsv},
    {"nvdec", DecodeBackend::NvidiaNvdec},
    {"cpu", DecodeBackend::Cpu},
};

} // namespace

std::string_view decode_backend_name(DecodeBackend backend) noexcept
{
    switch (backend) {
    case DecodeBackend::Auto:
        return "auto";
    case DecodeBackend::Vaapi:
        return "vaapi";
    case DecodeBackend::IntelQsv:
        return "qsv";
    case DecodeBackend::NvidiaNvdec:
        return "nvdec";
    case DecodeBackend::Cpu:
        return "cpu";
    }
    return "?";
}

std::string decode_selection_names()
{
    std::string names = "auto";
    for (const Entry& e : k_entries) {
        names += '|';
        names += e.name;
    }
    return names;
}

DecodeSelection decode_selection_from_name(std::string_view name)
{
    if (name == "auto") {
        // Hardware first, in the order this project has measured, and the CPU last so that
        // "auto" means "hardware if you have it" rather than "hardware or nothing". A
        // caller who wants the second spells it by naming a backend.
        return DecodeSelection{{DecodeBackend::Vaapi, DecodeBackend::IntelQsv,
            DecodeBackend::NvidiaNvdec, DecodeBackend::Cpu}};
    }
    for (const Entry& e : k_entries) {
        if (name == e.name) {
            // A named backend is a requirement, not a preference: the list holds it alone,
            // so a device that will not open is a failure the caller hears about rather
            // than a quiet demotion to software.
            return DecodeSelection{{e.backend}};
        }
    }
    throw std::invalid_argument(
        std::format("unknown decoder '{}', expected one of {}", name, decode_selection_names()));
}

} // namespace ta_roienc
