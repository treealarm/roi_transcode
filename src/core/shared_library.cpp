#include "shared_library.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace sve::platform {

#if defined(_WIN32)

SveBackendPluginEntryFn load_plugin_entry(
    const std::filesystem::path& path, std::string* error)
{
    const HMODULE module = ::LoadLibraryW(path.c_str());
    if (module == nullptr) {
        if (error != nullptr) {
            *error = "LoadLibrary failed with " + std::to_string(::GetLastError());
        }
        return nullptr;
    }
    const FARPROC symbol = ::GetProcAddress(module, kBackendPluginEntryPoint);
    if (symbol == nullptr) {
        if (error != nullptr) {
            *error = std::string("no ") + kBackendPluginEntryPoint + " in this module";
        }
        return nullptr;
    }
    return reinterpret_cast<SveBackendPluginEntryFn>(symbol);
}

std::filesystem::path executable_directory()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

std::string_view plugin_prefix() { return "sve_backend_"; }
std::string_view plugin_extension() { return ".dll"; }

#else

SveBackendPluginEntryFn load_plugin_entry(
    const std::filesystem::path& path, std::string* error)
{
    // RTLD_NOW so that an unbindable reference to the core is an error here rather than a
    // crash at the first call. RTLD_LOCAL because the module has exactly one symbol to
    // offer and no business interposing on anything else. RTLD_NODELETE says out loud what
    // this file's header explains: it is never coming back out.
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (handle == nullptr) {
        if (error != nullptr) {
            const char* why = ::dlerror();
            *error = why != nullptr ? why : "dlopen failed without saying why";
        }
        return nullptr;
    }
    ::dlerror();
    void* symbol = ::dlsym(handle, kBackendPluginEntryPoint);
    if (symbol == nullptr) {
        if (error != nullptr) {
            const char* why = ::dlerror();
            *error = why != nullptr ? why
                                    : std::string("no ") + kBackendPluginEntryPoint;
        }
        return nullptr;
    }
    // Casting an object pointer to a function pointer is conditionally supported by the
    // standard and required to work by POSIX, which is what dlsym returns one through.
    return reinterpret_cast<SveBackendPluginEntryFn>(symbol);
}

std::filesystem::path executable_directory()
{
    std::error_code ec;
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return self.parent_path();
}

std::string_view plugin_prefix() { return "libsve_backend_"; }
std::string_view plugin_extension() { return ".so"; }

#endif

} // namespace sve::platform
