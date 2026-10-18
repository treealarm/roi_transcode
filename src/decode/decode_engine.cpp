#include "ta_roienc/decode_engine.h"

#include <format>
#include <stdexcept>

namespace ta_roienc {

namespace {

/// Name, backend, and what naming it implies. One table, because the parser, the joined
/// name list and the reverse lookup are three views of the same fact and drifted apart on
/// the encoder side when they were three lists.
struct Slot {
    std::string_view name;
    DecodeEngine backend;
};

// "auto" is not here: it names an order rather than a backend, and is handled separately.
constexpr Slot k_entries[] = {
    {"vaapi", DecodeEngine::Vaapi},
    {"qsv", DecodeEngine::IntelQsv},
    {"nvdec", DecodeEngine::NvidiaNvdec},
    {"cpu", DecodeEngine::Cpu},
};

} // namespace

std::string_view decode_backend_name(DecodeEngine backend) noexcept
{
    switch (backend) {
    case DecodeEngine::Auto:
        return "auto";
    case DecodeEngine::Vaapi:
        return "vaapi";
    case DecodeEngine::IntelQsv:
        return "qsv";
    case DecodeEngine::NvidiaNvdec:
        return "nvdec";
    case DecodeEngine::Cpu:
        return "cpu";
    }
    return "?";
}

std::string decode_order_names()
{
    std::string names = "auto";
    for (const Slot& e : k_entries) {
        names += '|';
        names += e.name;
    }
    return names;
}

DecodeOrder decode_order_from_name(std::string_view name)
{
    if (name == "auto") {
        // Hardware first, in the order this project has measured, and the CPU last so that
        // "auto" means "hardware if you have it" rather than "hardware or nothing". A
        // caller who wants the second spells it by naming a backend.
        return DecodeOrder{{DecodeEngine::Vaapi, DecodeEngine::IntelQsv,
            DecodeEngine::NvidiaNvdec, DecodeEngine::Cpu}};
    }
    for (const Slot& e : k_entries) {
        if (name == e.name) {
            // A named backend is a requirement, not a preference: the list holds it alone,
            // so a device that will not open is a failure the caller hears about rather
            // than a quiet demotion to software.
            return DecodeOrder{{e.backend}};
        }
    }
    throw std::invalid_argument(
        std::format("unknown decoder '{}', expected one of {}", name, decode_order_names()));
}

} // namespace ta_roienc
