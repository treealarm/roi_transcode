// Which backends to try, and in what order -- TZ v3.4 §8.4.
//
// One rule, not two. A named Encoder is the short way to say "only this one", and it is
// expressed as a selection of one rather than as a separate code path -- otherwise the
// ordering lives in two places and they drift.
//
// A named kind also means it: falling back to libx264 when the caller asked for hardware
// would hide the very thing they were checking. That is why a named kind ignores
// allow_unlisted and allow_software_fallback -- the caller has already been explicit.

#include "sve/backend_selection.h"
#include "sve/encoder_backend.h"

#include <algorithm>
#include <vector>

namespace sve {

const IEncoderBackend& onevpl_backend();
const IEncoderBackend& vaapi_backend();
const IEncoderBackend& nvenc_backend();
const IEncoderBackend& software_backend();

namespace {

const IEncoderBackend* backend_for(Encoder kind)
{
    switch (kind) {
    case Encoder::Qsv:   return &onevpl_backend();
    case Encoder::Vaapi: return &vaapi_backend();
    case Encoder::Nvenc: return &nvenc_backend();
    case Encoder::Cpu:   return &software_backend();
    case Encoder::Auto:  break;
    }
    return nullptr;
}

/// Every backend this build has, in the order Auto would consider them if nothing was
/// excluded. Used only to honour allow_unlisted.
constexpr Encoder kAllBackends[] = {
    Encoder::Qsv, Encoder::Vaapi, Encoder::Nvenc, Encoder::Cpu};

} // namespace

std::vector<BackendRef> resolve_backends(Encoder requested, const BackendSelection& selection)
{
    std::vector<BackendRef> out;

    if (requested != Encoder::Auto) {
        if (const IEncoderBackend* one = backend_for(requested)) {
            out.emplace_back(*one);
        }
        return out;
    }

    const auto append = [&](Encoder kind) {
        if (kind == Encoder::Cpu && !selection.allow_software_fallback) {
            return;
        }
        const IEncoderBackend* backend = backend_for(kind);
        if (backend == nullptr) {
            return;
        }
        const bool already = std::ranges::any_of(out, [&](const BackendRef& seen) {
            return &seen.get() == backend;
        });
        if (!already) {
            out.emplace_back(*backend);
        }
    };

    for (const Encoder kind : selection.preferred) {
        append(kind);
    }
    if (selection.allow_unlisted) {
        // The listed ones first, then whatever else this build has -- "unlisted allowed"
        // is a permission to keep going, not a licence to reorder.
        for (const Encoder kind : kAllBackends) {
            append(kind);
        }
    }
    return out;
}

} // namespace sve
