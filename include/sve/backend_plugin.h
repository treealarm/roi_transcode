#pragma once

// The contract a backend component answers to -- TZ v4.2 §26.4.
//
// §26.4 asks for two ways to deliver the same backend: linked into the deployment and
// registered explicitly, or shipped as a shared module and found at runtime. And it settles
// the design in one sentence:
//
//     Static и dynamic варианты одного backend используют одну реализацию и один
//     SveBackendPluginDescriptorV1.
//
// So this is not the plugin's format, it is the component's, and the static path reads the
// same one: RegisterX264Backend hands the registry exactly the descriptor that
// sve_backend_plugin_v1() returns from the dynamic build of the same sources. Two
// descriptions of one backend could disagree, and the disagreement would surface only in
// whichever packaging profile nobody happened to build that day.
//
// It is a C ABI in the sense that matters here: C linkage on the entry point, a
// standard-layout struct whose first two fields never move, and no exception crossing it.
// It is not a C API -- what it yields is IEncoderBackend, the C++ interface of §21, because
// that is what this library's public surface is, and inventing a C mirror of it would be a
// second interface to keep in step with the first. A plugin and the core that loads it are
// built by the same toolchain against the same headers; abi_version is what says so out
// loud, and a mismatch is refused rather than dereferenced.

#include "sve/backend_selection.h"

#include <cstdint>

#if defined(_WIN32)
#  define SVE_BACKEND_EXPORT __declspec(dllexport)
#else
#  define SVE_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

namespace sve {

class IEncoderBackend;

/// Bumped whenever a field moves, changes meaning, or is added, and checked before any
/// other field is read. It is the first field for that reason: a descriptor from a build
/// that disagrees with this one about the layout must still be able to say so.
///
/// It guards the interface behind the descriptor as well, not only the descriptor's own
/// fields. What a plugin hands over is an IEncoderBackend, so a virtual added to that class
/// changes the table a plugin built against the older header produces -- a mismatch that no
/// field of this struct would show. Version 2 is that: `GetEffectiveSettings` (2026-09-03).
///
/// It guards the vocabulary those calls are made in as well. Version 3 is that: PixelFormat
/// gained YUV420P10 between P010 and HardwareSurface, so every value after it means
/// something different than it did, and a plugin that still counts the old way would take a
/// hardware surface for a planar one (2026-09-07).
inline constexpr uint32_t kBackendPluginAbiVersion = 3;

/// Hands over the component's backend, or null if it cannot be had.
///
/// Called once per registration, not per encode, and never throws -- §26.4 puts the
/// no-exceptions rule on the boundary, and a component that lets one out here would be
/// unwinding through a frame the core cannot see.
using SveBackendAcquireFn = const IEncoderBackend* (*)() noexcept;

/// What a component says about itself. Every field is one §26.4 names.
struct SveBackendPluginDescriptorV1 {
    /// kBackendPluginAbiVersion as the component saw it.
    uint32_t abi_version;

    /// sizeof(SveBackendPluginDescriptorV1) as the component saw it. Checked as well as
    /// the version because the two catch different mistakes: a version that matches while
    /// the size does not means one side was rebuilt and the other was not, and reading the
    /// fields below on that assumption is reading past the object.
    uint32_t descriptor_size;

    /// The Backend enumerator this component provides, as a fixed-width integer rather than
    /// as the enum: the enum's underlying type is the implementation's choice, and a field
    /// in a layout contract cannot be.
    ///
    /// Checked against what the backend itself reports, so a descriptor claiming one
    /// identifier while acquiring another is refused instead of being registered under a
    /// name that answers with somebody else's capabilities.
    int32_t backend_id;

    SveBackendAcquireFn acquire;
};

/// What a dynamic plugin exports, and the only symbol of it the core looks for.
using SveBackendPluginEntryFn = const SveBackendPluginDescriptorV1* (*)() noexcept;

/// The name of that symbol. One definition, because the loader looks it up by this string
/// and every plugin defines it -- a second spelling would be a plugin that loads nowhere.
inline constexpr const char* kBackendPluginEntryPoint = "sve_backend_plugin_v1";

} // namespace sve
