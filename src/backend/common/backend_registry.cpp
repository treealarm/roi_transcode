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

#include "sve/backend_registry.h"
#include "sve/backend_selection.h"
#include "sve/encoder_backend.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace sve {

Status BackendRegistry::Register(const SveBackendPluginDescriptorV1& descriptor) noexcept
{
    // Checked in this order on purpose: the version says whether the rest of the object
    // means what this build thinks it means, so nothing below it is read until it agrees.
    if (descriptor.abi_version != kBackendPluginAbiVersion) {
        return Status::InvalidArgument(std::format(
            "backend plugin abi version {} does not match this build's {}",
            descriptor.abi_version, kBackendPluginAbiVersion));
    }
    if (descriptor.descriptor_size != sizeof(SveBackendPluginDescriptorV1)) {
        return Status::InvalidArgument(std::format(
            "backend plugin descriptor is {} bytes, this build's is {}",
            descriptor.descriptor_size, sizeof(SveBackendPluginDescriptorV1)));
    }
    if (descriptor.acquire == nullptr) {
        return Status::InvalidArgument("backend plugin descriptor has no acquire function");
    }
    const IEncoderBackend* acquired = descriptor.acquire();
    if (acquired == nullptr) {
        return Status::InvalidArgument("backend plugin acquired no backend");
    }
    const Backend kind = acquired->kind();
    if (static_cast<int32_t>(kind) != descriptor.backend_id) {
        return Status::InvalidArgument(std::format(
            "backend plugin declares identifier {} and acquired {}",
            descriptor.backend_id, backend_name(kind)));
    }
    const IEncoderBackend& backend = *acquired;

    for (const IEncoderBackend* seen : m_backends) {
        if (seen->kind() == kind) {
            // §26.4: a duplicate identifier is refused, never silently overridden. The
            // same backend registered twice is a deployment that linked a component and
            // loaded its plugin, and picking a winner there would be picking at random.
            return Status::InvalidArgument(
                std::format("backend {} is already registered", backend_name(kind)));
        }
    }
    m_backends.push_back(&backend);
    return Status::Ok();
}

const IEncoderBackend* BackendRegistry::Find(Backend kind) const noexcept
{
    for (const IEncoderBackend* seen : m_backends) {
        if (seen->kind() == kind) {
            return seen;
        }
    }
    return nullptr;
}

std::vector<Backend> BackendRegistry::kinds() const
{
    std::vector<Backend> out;
    out.reserve(m_backends.size());
    for (const IEncoderBackend* seen : m_backends) {
        out.push_back(seen->kind());
    }
    return out;
}

BackendRegistry& default_backend_registry()
{
    // Filled once, on first use, from whatever the build generated. Not a global
    // constructor: those are what §26.4 forbids relying on, and the ordering between one
    // in the core and one in a component is exactly the thing nobody can pin down.
    static BackendRegistry registry = [] {
        BackendRegistry fresh;
        (void)RegisterDeploymentBackends(fresh);
        return fresh;
    }();
    return registry;
}

namespace {

const IEncoderBackend* backend_for(Backend kind)
{
    return default_backend_registry().Find(kind);
}

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
        // is a permission to keep going, not a licence to reorder. "Whatever else" is now
        // what was registered rather than a constant list, which is the point: a build
        // without a component must not offer it.
        for (const Backend kind : default_backend_registry().kinds()) {
            append(kind);
        }
    }
    return out;
}

} // namespace sve
