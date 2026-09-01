// Where plugins are looked for, and what happens to the ones that should not load --
// TZ v4.2 §26.4.
//
// Every case here uses a real shared module built by this test's own CMake, because the
// whole subject is what dlopen and a descriptor do when they meet, and a fake of either
// would be testing the fake. The four modules live in four directories so that a test can
// say exactly what is in scope -- which is also how the search order gets checked, by
// putting the same identifier in two of them and seeing which one wins.

#include "sve/backend_loader.h"
#include "sve/backend_registry.h"
#include "sve/encoder_backend.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace sve;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "plugin_loader_test: %s\n", what.c_str());
        ++failures;
    }
}

std::filesystem::path plugin_dir(const char* name)
{
    return std::filesystem::path(SVE_TEST_PLUGIN_DIR) / name;
}

/// Only the directories named, so that nothing installed on the machine running this can
/// wander into the result.
PluginSearch only(std::vector<std::filesystem::path> paths)
{
    PluginSearch search;
    search.paths = std::move(paths);
    search.install_relative = false;
    search.system = false;
    return search;
}

bool mentions(const std::vector<std::string>& lines, const std::string& needle)
{
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void nothing_is_searched_unless_it_is_asked_for()
{
    // §26.4 keeps the working directory out of the search, and this is the strong form of
    // that: with no source enabled, nothing at all is looked at. A default that had to be
    // switched off would be one an environment variable could switch back on.
    BackendRegistry registry;
    const PluginLoadReport report = LoadBackendPlugins(registry, only({}));
    check(report.searched.empty(), "a search with no sources looked somewhere");
    check(report.loaded.empty(), "a search with no sources loaded something");
}

void the_order_is_the_one_the_specification_fixes()
{
    // Explicit paths in the order given, then install-relative, then system-configured.
    BackendRegistry registry;
    PluginSearch search;
    search.paths = {plugin_dir("no_entry"), plugin_dir("bad_abi")};
    const PluginLoadReport report = LoadBackendPlugins(registry, search);

    check(report.searched.size() == 4,
        "expected four directories, got " + std::to_string(report.searched.size()));
    check(report.searched[0].find("no_entry") != std::string::npos,
        "the first explicit path was not searched first");
    check(report.searched[1].find("bad_abi") != std::string::npos,
        "the second explicit path was not searched second");
}

void a_directory_that_is_not_there_is_not_a_failure()
{
    // §26.4: missing optional plugins are safe. A deployment that ships none must start.
    BackendRegistry registry;
    const PluginLoadReport report =
        LoadBackendPlugins(registry, only({plugin_dir("no-such-directory")}));
    check(report.loaded.empty(), "something was loaded from a directory that is not there");
    check(report.rejected.empty(), "an absent directory was reported as a refusal");
    check(report.searched.size() == 1, "the absent directory was not reported as searched");
}

void a_good_plugin_registers_and_is_findable()
{
    BackendRegistry registry;
    const PluginLoadReport report = LoadBackendPlugins(registry, only({plugin_dir("good")}));
    check(report.loaded.size() == 1,
        "expected one plugin, got " + std::to_string(report.loaded.size()));
    check(report.rejected.empty(),
        report.rejected.empty() ? "" : "a good plugin was refused: " + report.rejected[0]);
    if (!report.loaded.empty()) {
        check(report.loaded[0] == Backend::NvidiaNvenc, "the wrong backend was registered");
    }
    const IEncoderBackend* found = registry.Find(Backend::NvidiaNvenc);
    check(found != nullptr, "the loaded backend is not in the registry");
    if (found != nullptr) {
        // Through the C ABI boundary and back: the object answers, which is the whole
        // claim. What it answers is the stub's business.
        check(found->kind() == Backend::NvidiaNvenc,
            std::string("the loaded backend answers ") + backend_name(found->kind()));
    }
}

void a_version_from_another_build_is_refused_by_name()
{
    BackendRegistry registry;
    const PluginLoadReport report =
        LoadBackendPlugins(registry, only({plugin_dir("bad_abi")}));
    check(report.loaded.empty(), "a plugin with an unknown abi version was registered");
    check(report.rejected.size() == 1, "the bad plugin was not reported");
    check(mentions(report.rejected, "abi version"),
        "the refusal does not say the abi version was wrong");
    check(mentions(report.rejected, "bad_abi"), "the refusal does not name the file");
}

void a_module_without_the_entry_point_is_refused_by_name()
{
    BackendRegistry registry;
    const PluginLoadReport report =
        LoadBackendPlugins(registry, only({plugin_dir("no_entry")}));
    check(report.loaded.empty(), "a module with no entry point registered something");
    check(report.rejected.size() == 1, "the module with no entry point was not reported");
    check(mentions(report.rejected, "sve_backend_plugin_v1"),
        "the refusal does not name the symbol it wanted");
}

void the_same_identifier_from_two_directories_is_refused_once()
{
    // §26.4's duplicate case, and the reason the search order has to be deterministic: the
    // directory that comes first is the one whose plugin is used, on every machine.
    BackendRegistry registry;
    const PluginLoadReport report =
        LoadBackendPlugins(registry, only({plugin_dir("good"), plugin_dir("dup")}));
    check(report.loaded.size() == 1, "a duplicate identifier was registered twice");
    check(report.rejected.size() == 1, "the duplicate was not reported");
    check(mentions(report.rejected, "already registered"),
        "the duplicate refusal does not say so");
    check(mentions(report.rejected, "dup"),
        "the refusal blames the wrong file: the first directory should win");
}

void a_plugin_of_a_backend_already_linked_in_is_refused()
{
    // The case §26.4 calls out by hand -- "включая конфликт static и dynamic вариантов".
    // Registering the stub first stands in for a component linked into the deployment.
    BackendRegistry registry;
    const PluginLoadReport first =
        LoadBackendPlugins(registry, only({plugin_dir("good")}));
    check(first.loaded.size() == 1, "the first load did not take");

    const PluginLoadReport second =
        LoadBackendPlugins(registry, only({plugin_dir("good")}));
    check(second.loaded.empty(), "the same plugin registered twice");
    check(mentions(second.rejected, "already registered"),
        "the second load did not refuse the duplicate");
}

} // namespace

int main()
{
    nothing_is_searched_unless_it_is_asked_for();
    the_order_is_the_one_the_specification_fixes();
    a_directory_that_is_not_there_is_not_a_failure();
    a_good_plugin_registers_and_is_findable();
    a_version_from_another_build_is_refused_by_name();
    a_module_without_the_entry_point_is_refused_by_name();
    the_same_identifier_from_two_directories_is_refused_once();
    a_plugin_of_a_backend_already_linked_in_is_refused();

    if (failures != 0) {
        std::fprintf(stderr, "plugin_loader_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("plugin_loader_test ok");
    return 0;
}
