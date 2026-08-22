# Container images — TZ v2.0 §52

`intel/` holds what exists: the vcpkg toolchain base and the runtime image with the Intel
media driver and the oneVPL GPU runtime.

`cpu/`, `nvidia/` and `rockchip/` are empty. A software-only image is easy but nothing
needs it yet — `linux-software-only` in CMakePresets.json covers building without hardware.
