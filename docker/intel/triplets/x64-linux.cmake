set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Overlay of vcpkg's builtin x64-linux triplet: VCPKG_BUILD_TYPE only takes effect from inside a
# triplet file (setting it as an env var when invoking `vcpkg install` is a silent no-op), so
# without this vcpkg builds full debug variants of every port too — e.g. libgrpc.a balloons from
# ~50MB (release) to 2GB+ (debug), which is enough on its own to OOM a linker consuming it.
set(VCPKG_BUILD_TYPE release)
