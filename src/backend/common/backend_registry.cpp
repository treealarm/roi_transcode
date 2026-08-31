// Which backends to try, and in what order -- TZ v4.1 §8.4.
//
// One rule, not two. A named Backend is the short way to say "only this one", and it is
// expressed as a selection of one rather than as a separate code path -- otherwise the
// ordering lives in two places and they drift.
//
// A named kind also means it: falling back to libx264 when the caller asked for hardware
// would hide the very thing they were checking. That is why a named kind ignores
// allow_unlisted -- the caller has already been explicit.
//
// Two things changed in 4.1. There is no allow_software_fallback switch: the software
// encoders are ordinary entries, and leaving them out of the list is how a caller says
// "hardware or nothing". And selection now knows the codec, because X264 and X265 are
// separate backends -- one that cannot produce what was asked for is skipped here, with
// its reason recorded, rather than opened and found wanting.

#include "sve/backend_selection.h"
#include "sve/encoder_backend.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace sve {

const IEncoderBackend& onevpl_backend();
const IEncoderBackend& vaapi_backend();
const IEncoderBackend& nvenc_backend();
const IEncoderBackend& x264_backend();
const IEncoderBackend& x265_backend();

namespace {

const IEncoderBackend* backend_for(Backend kind)
{
    switch (kind) {
    case Backend::IntelOneVpl:   return &onevpl_backend();
    case Backend::Vaapi: return &vaapi_backend();
    case Backend::NvidiaNvenc: return &nvenc_backend();
    case Backend::X264:  return &x264_backend();
    case Backend::X265:  return &x265_backend();
    case Backend::Auto:  break;
    }
    return nullptr;
}

/// Every backend this build has, in the order Auto would consider them if nothing was
/// excluded. Used only to honour allow_unlisted -- it is not a default preference order,
/// which 4.1 removed on purpose.
constexpr Backend kAllBackends[] = {
    Backend::IntelOneVpl, Backend::Vaapi, Backend::NvidiaNvenc, Backend::X264, Backend::X265};

} // namespace

bool backend_supports(Backend kind, Codec codec)
{
    const IEncoderBackend* backend = backend_for(kind);
    if (backend == nullptr) {
        return false;
    }
    const EncoderCapabilities caps = backend->GetCapabilities(codec);
    return codec == Codec::H264 ? caps.h264 : caps.h265;
}

std::vector<BackendRef> resolve_backends(Backend requested, const BackendSelection& selection,
    Codec codec, std::vector<std::string>* skipped)
{
    std::vector<BackendRef> out;

    // Reported once per backend. allow_unlisted walks the full list after the caller's
    // own, so without this a backend named in `preferred` and skipped there would be
    // reported again on the second pass.
    std::vector<Backend> noted;
    const auto note_skip = [&](Backend kind) {
        if (skipped == nullptr || std::ranges::find(noted, kind) != noted.end()) {
            return;
        }
        noted.push_back(kind);
        skipped->push_back(std::format("{} cannot encode {}", backend_name(kind),
            codec == Codec::H264 ? "H.264" : "HEVC"));
    };

    if (requested != Backend::Auto) {
        const IEncoderBackend* one = backend_for(requested);
        if (one == nullptr) {
            return out;
        }
        // Named outright and unable to serve the codec: §8.4 wants Unsupported, so the
        // chain comes back empty and the reason travels with it.
        if (!backend_supports(requested, codec)) {
            note_skip(requested);
            return out;
        }
        out.emplace_back(*one);
        return out;
    }

    const auto append = [&](Backend kind) {
        const IEncoderBackend* backend = backend_for(kind);
        if (backend == nullptr) {
            return;
        }
        const bool already = std::ranges::any_of(out, [&](const BackendRef& seen) {
            return &seen.get() == backend;
        });
        if (already) {
            return;
        }
        if (!backend_supports(kind, codec)) {
            note_skip(kind);
            return;
        }
        out.emplace_back(*backend);
    };

    for (const Backend kind : selection.preferred) {
        append(kind);
    }
    if (selection.allow_unlisted) {
        // The listed ones first, then whatever else this build has -- "unlisted allowed"
        // is a permission to keep going, not a licence to reorder.
        for (const Backend kind : kAllBackends) {
            append(kind);
        }
    }
    return out;
}

} // namespace sve
