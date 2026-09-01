#include "sve/backend_loader.h"

#include "shared_library.h"

#include <algorithm>
#include <format>
#include <system_error>

namespace sve {

namespace {

namespace fs = std::filesystem;

/// Where this build installs its plugins, and where they sit relative to a program that was
/// installed beside them. Both come from the build rather than from a constant here: the
/// install layout is CMake's to decide, and a second copy of it would be the kind of
/// meaningful number that drifts.
constexpr const char* kSystemPluginDir = SVE_PLUGIN_SYSTEM_DIR;
constexpr const char* kRelativePluginDir = SVE_PLUGIN_RELATIVE_DIR;

bool looks_like_a_plugin(const fs::path& path)
{
    const std::string name = path.filename().string();
    return name.starts_with(platform::plugin_prefix())
        && path.extension() == platform::plugin_extension();
}

void load_one(const fs::path& path, BackendRegistry& registry, PluginLoadReport& report)
{
    std::string error;
    const SveBackendPluginEntryFn entry = platform::load_plugin_entry(path, &error);
    if (entry == nullptr) {
        report.rejected.push_back(std::format("{}: {}", path.string(), error));
        return;
    }
    const SveBackendPluginDescriptorV1* descriptor = entry();
    if (descriptor == nullptr) {
        report.rejected.push_back(
            std::format("{}: the entry point returned no descriptor", path.string()));
        return;
    }
    // Everything §26.4 asks to be checked is checked in one place, by the same code the
    // static path goes through. A second set of checks here would be a second thing to keep
    // correct, and the one that got it wrong would be whichever nobody exercised.
    if (const Status s = registry.Register(*descriptor); !s.ok()) {
        report.rejected.push_back(std::format("{}: {}", path.string(), s.message()));
        return;
    }
    report.loaded.push_back(static_cast<Backend>(descriptor->backend_id));
}

void scan(const fs::path& dir, BackendRegistry& registry, PluginLoadReport& report)
{
    report.searched.push_back(dir.string());

    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        // Not there is not a failure: §26.4 requires that optional plugins may simply be
        // absent, and a deployment that ships none must still start.
        return;
    }

    std::vector<fs::path> found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (looks_like_a_plugin(it->path())) {
            found.push_back(it->path());
        }
    }
    std::ranges::sort(found);
    for (const fs::path& path : found) {
        load_one(path, registry, report);
    }
}

} // namespace

PluginLoadReport LoadBackendPlugins(BackendRegistry& registry, const PluginSearch& search)
{
    PluginLoadReport report;

    for (const fs::path& dir : search.paths) {
        scan(dir, registry, report);
    }
    if (search.install_relative) {
        const fs::path exe = platform::executable_directory();
        if (!exe.empty()) {
            // lexically_normal so the report names a directory a human can go and look in,
            // rather than one with a ".." in the middle of it.
            scan((exe / kRelativePluginDir).lexically_normal(), registry, report);
        }
    }
    if (search.system) {
        scan(fs::path(kSystemPluginDir), registry, report);
    }
    return report;
}

} // namespace sve
