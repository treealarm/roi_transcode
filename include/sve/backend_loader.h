#pragma once

// Finding backend plugins and registering what they describe -- TZ v4.2 §26.4.
//
// The other half of the descriptor in sve/backend_plugin.h. A component linked into the
// deployment registers itself by a call the build generated; a component shipped as a
// shared module is found here, and both arrive at BackendRegistry::Register with the same
// descriptor, which is what §26.4 means by one implementation and one contract.
//
// §26.4 fixes the order they are looked for in:
//
//     Порядок поиска plugins детерминирован: explicit application path, install-relative
//     directory, затем system-configured directory; current working directory по умолчанию
//     не используется.
//
// The working directory is absent from this header entirely rather than defaulted off,
// because a search path that can be turned on by an environment variable is one an attacker
// can turn on too. An application that genuinely wants to load from somewhere says so by
// naming the directory.

#include "sve/backend_registry.h"
#include "sve/backend_selection.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sve {

/// Where to look, in the order §26.4 lays down.
struct PluginSearch {
    /// Directories the application names outright. Searched first, in this order, and the
    /// only way to reach a directory this build was not configured with.
    std::vector<std::filesystem::path> paths;

    /// Beside the running program, at the install layout this build was configured for.
    /// True by default: a deployment that installed the plugins next to the binary expects
    /// them to be found without being told where they are.
    bool install_relative = true;

    /// The directory this build was configured to install plugins into. True by default,
    /// and the last place looked, so a plugin beside the program wins over one installed
    /// system-wide -- which is the way round that lets a deployment override without
    /// uninstalling anything.
    bool system = true;
};

/// What a search found, and what it refused.
///
/// A report rather than a Status because the call has no single answer to give. Finding
/// nothing is success -- §26.4 requires that missing optional plugins are safe -- while a
/// plugin that was found and refused is neither success nor a reason to leave the process
/// with no backends at all. §50 is met by the refusal being reported, not by it being
/// fatal.
struct PluginLoadReport {
    /// Registered, in the order they were found.
    std::vector<Backend> loaded;

    /// One line per plugin found and refused, naming the file and the reason: a version
    /// this build does not know, a descriptor of the wrong size, an identifier already
    /// taken by a component linked in, a module that will not load at all.
    std::vector<std::string> rejected;

    /// Every directory looked in, in order, whether or not it existed. Here because "which
    /// directory did this one come from" is the first question asked when the wrong plugin
    /// loads, and §22 does not treat that as optional.
    std::vector<std::string> searched;
};

/// Loads every backend plugin the search finds, registering each into `registry`.
///
/// Within one directory the files are taken in sorted order, so that two machines with the
/// same plugins installed register them in the same sequence -- the order a directory
/// iterator hands them back is the filesystem's business and differs between runs.
///
/// **Call it before the first encoder exists, and from one thread.** It writes to the
/// registry, which everything that opens a backend then reads, and this library synchronises
/// neither -- a registry is assembled once and read for the life of the process, which is
/// the shape a deployment actually has. Loading plugins beside a running encode is the one
/// way to break that, and it is a caller's mistake rather than something to lock against:
/// the lock would cost every frame to protect a call that happens at startup.
PluginLoadReport LoadBackendPlugins(BackendRegistry& registry, const PluginSearch& search);

} // namespace sve
