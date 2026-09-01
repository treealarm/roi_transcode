#!/usr/bin/env bash
# Dependency scan -- TZ v4.2 §26.4 and §44.
#
# The specification states what the core may not depend on:
#
#     Основной sve.pc не объявляет FFmpeg, x264, x265 или vendor SDK зависимостями.
#     Component metadata объявляет только собственные зависимости соответствующего backend.
#
# Two things are checked, because a declaration and a link are different claims. The
# generated .pc files say what each unit admits to needing; readelf says what it actually
# pulls. A core whose .pc is clean and whose .so links libavcodec would pass the first and
# fail the point of it.
#
# Shared libraries are configured on purpose: readelf has nothing to read in a static
# archive, which is why this check did not exist before the §5.5 split. The default build
# stays static because ta_vms links us that way.
#
# The profile is §5.5's normative graph -- core, the two direct codec components and the
# FFmpeg interop -- and the examples and libavcodec-backed backends are off. Not to make
# the numbers look better: vcpkg builds FFmpeg as a static library without -fPIC, so
# anything linking libavcodec cannot become a shared object on this machine at all. The
# units the scan is about link fine, which is the point; a plugin built on libavcodec would
# need a PIC FFmpeg, and that is worth knowing before the dynamic slice tries.
#
#   ./integration/scripts/dependency-scan.sh [build-dir]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-${ROOT}/build-depscan}"
PREFIX="${BUILD}/prefix"
fails=0
checked=0
pass() { printf '  ok    %s\n' "$1"; checked=$((checked + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); checked=$((checked + 1)); }
# A skip is not a pass. The first version of this script reported success having skipped
# every readelf check, because the build it configured produced static archives and there
# was no shared object to read -- the same shape of false green CLAUDE.md warns about for
# the synthetic scenes. Skipped work is counted and a run that scanned nothing fails.
skipped=0
skip() { printf '  skip  %s\n' "$1"; skipped=$((skipped + 1)); }

echo "== configuring a shared build =="
rm -rf "$BUILD"
mkdir -p "$BUILD"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-/mnt/wdblue/vcpkg}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_MODE=OFF -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DSVE_BUILD_TESTS=OFF \
    -DSVE_BUILD_EXAMPLES=OFF -DSVE_BUILD_BACKEND_FFMPEG=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" >"${BUILD}/configure.log" 2>&1 || {
        echo "  configure failed; see ${BUILD}/configure.log" >&2; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" >"${BUILD}/build.log" 2>&1 || {
        echo "  build failed; see ${BUILD}/build.log" >&2; exit 1; }
cmake --install "$BUILD" >"${BUILD}/install.log" 2>&1 || {
        echo "  install failed; see ${BUILD}/install.log" >&2; exit 1; }

# What a shared object actually needs, by its own DT_NEEDED entries.
needed() { readelf -d "$1" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'; }

# `label` must not link anything matching `pattern`.
forbid() { # file label pattern what
    local file="$1" label="$2" pattern="$3" what="$4"
    [[ -f "$file" ]] || { skip "${label} (not built)"; return; }
    local hit; hit=$(needed "$file" | grep -E "$pattern" | tr '\n' ' ')
    if [[ -n "$hit" ]]; then fail "${label} links ${what}: ${hit}"; else pass "${label}: no ${what}"; fi
}

LIB="${PREFIX}/lib"
echo
echo "== §26.4: the core links nothing it must not =="
forbid "${LIB}/libsve.so"              "core"        'libav|libsw'      "FFmpeg"
forbid "${LIB}/libsve.so"              "core"        'libx264|libx265'  "codec library"
forbid "${LIB}/libsve.so"              "core"        'libva|libmfx|libvpl|libcuda|libnvidia' "vendor SDK"

echo
echo "== §44: each codec component links only its own codec =="
forbid "${LIB}/libsve_backend_x264.so" "x264 backend" 'libav|libsw'     "FFmpeg"
forbid "${LIB}/libsve_backend_x264.so" "x264 backend" 'libx265'         "libx265"
forbid "${LIB}/libsve_backend_x265.so" "x265 backend" 'libav|libsw'     "FFmpeg"
forbid "${LIB}/libsve_backend_x265.so" "x265 backend" 'libx264'         "libx264"

echo
echo "== §26.4: the FFmpeg interop converts frames and does not encode =="
forbid "${LIB}/libsve_ffmpeg_interop.so" "interop"    'libavcodec|libavformat' "libavcodec or libavformat"
forbid "${LIB}/libsve_ffmpeg_interop.so" "interop"    'libx264|libx265'        "codec library"

echo
echo "== §26.4: what the generated pkg-config files declare =="
pc_requires() { sed -n 's/^Requires.private:[[:space:]]*//p' "$1"; }
core_pc="${LIB}/pkgconfig/sve.pc"
if [[ -f "$core_pc" ]]; then
    got=$(pc_requires "$core_pc")
    if [[ -z "${got// }" ]]; then pass "sve.pc Requires.private is empty"
    else fail "sve.pc declares: ${got}"; fi
else
    fail "sve.pc was not installed"
fi
for spec in "sve-backend-x264.pc x264" "sve-backend-x265.pc x265" "sve-ffmpeg-interop.pc libavutil"; do
    set -- $spec
    f="${LIB}/pkgconfig/$1"
    [[ -f "$f" ]] || { skip "$1 (not built)"; continue; }
    got=$(pc_requires "$f")
    if [[ "$got" == "$2" ]]; then pass "$1 declares only $2"
    else fail "$1 declares '${got}', expected '$2'"; fi
done

echo
# The core is always built, so its three readelf checks always run. If they did not, the
# scan measured nothing and must say so rather than print a green line.
if [[ $checked -lt 4 ]]; then
    echo "dependency scan ran only ${checked} check(s) and skipped ${skipped}: nothing was scanned"
    exit 1
fi
if [[ $fails -eq 0 ]]; then
    echo "dependency scan passed (${checked} checks, ${skipped} skipped)"
else
    echo "${fails} check(s) failed (${checked} checks, ${skipped} skipped)"
fi
exit $(( fails > 0 ))
