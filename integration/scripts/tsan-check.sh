#!/usr/bin/env bash
# The concurrency test under ThreadSanitizer.
#
# Separate from check.sh and separate from the ordinary build, for two reasons. It needs its
# own build tree -- every translation unit has to be instrumented, and mixing instrumented
# and uninstrumented objects in one tree is how a TSan run reports nothing and means nothing.
# And it is slow: TSan costs roughly 5-15x, which is fine for one test and not for 28 steps.
#
# What it is for. tests/unit/concurrency_test passing is weak evidence: a data race can stay
# invisible for a thousand runs because it needs an interleaving that did not happen. TSan
# does not wait for the interleaving -- it watches the accesses and the happens-before edges
# and reports the race the first time both sides run at all. That is the difference between
# "we did not see it" and "it is not there".
#
# Known and deliberate: libx264, libx265 and the oneVPL runtime are not instrumented. TSan
# still follows their pthread calls through its interceptors, so ordering it can see is
# honoured, but a race entirely inside one of them will not be reported. This checks our
# code, which is what it claims to check.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TSAN_BUILD:-${ROOT}/build-tsan}"
VCPKG="${VCPKG_ROOT:-/mnt/wdblue/vcpkg}"

if [[ ! -f "${VCPKG}/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "no vcpkg toolchain at ${VCPKG}; set VCPKG_ROOT" >&2
    exit 1
fi

# Configured from scratch every time, never reused. pkg_check_modules caches its answer in
# CMakeCache.txt and never asks pkg-config again, so a tree carried over from a different
# feature set links against a line that no longer describes the libraries on disk.
rm -rf "${BUILD}"
mkdir -p "${BUILD}"
CONFLOG="${BUILD}/configure.log"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSVE_BUILD_INTEGRATION=OFF \
    -DSVE_BUILD_BACKEND_FFMPEG=OFF \
    -DSVE_BUILD_BACKEND_ONEVPL=ON \
    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >"${CONFLOG}" 2>&1 \
    || { echo "configure failed; see ${CONFLOG}" >&2; tail -20 "${CONFLOG}" >&2; exit 1; }

cmake --build "${BUILD}" --target concurrency_test -j"$(nproc)" || exit 1

# halt_on_error keeps the exit code meaningful: without it TSan prints the report and the
# test still exits 0, which is a green run with a race in it.
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
    "${BUILD}/concurrency_test"
rc=$?
if [[ $rc -eq 0 ]]; then
    printf '  ok    concurrency_test is clean under ThreadSanitizer\n'
else
    printf '  FAIL  ThreadSanitizer reported a problem (exit %d)\n' "$rc"
fi
exit $rc
