#pragma once

// Loading a shared module, and the two platform calls that needs -- TZ v4.2 §26.4.
//
// The one place in this library that is written twice for two operating systems, and it is
// here rather than behind a backend because there is nothing else it could be behind: a
// dynamic plugin loader is the platform, and §26.4 asks for one. Everything above this
// header sees one signature. `std::filesystem` does the rest, so no dirent.h and no
// realpath appear anywhere (repository decision Р2).
//
// It hands back the entry point rather than a handle, and that is the lifetime rule made
// structural. §26.4 says a plugin must not be unloaded while a backend or session it
// created is alive; our backends are immortal stateless singletons living inside the module
// (Р7), so there is no moment at which unloading would be safe and nothing counts sessions
// to find one. A handle nobody can close cannot be closed at the wrong time.

#include "sve/backend_plugin.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace sve::platform {

/// Loads `path` and returns its §26.4 entry point, or null with `error` saying why.
///
/// Resolves eagerly: a module whose references to the core cannot be bound must fail here,
/// where the reason can be reported, and not at the first call into it. That is the visible
/// half of this project's plugin arrangement -- the core is not inside the module, so a
/// host that did not export its own symbols is a deployment mistake, and this is where it
/// says so instead of crashing later.
SveBackendPluginEntryFn load_plugin_entry(
    const std::filesystem::path& path, std::string* error);

/// The directory the running program lives in, or empty when it cannot be determined.
std::filesystem::path executable_directory();

/// What a backend plugin is called here: the prefix its name starts with and the extension
/// it ends with. §26.4 calls the exact spelling a packaging detail, which it is -- but the
/// loader still has to recognise one, and this is the only place that decides how.
std::string_view plugin_prefix();
std::string_view plugin_extension();

} // namespace sve::platform
