#!/usr/bin/env bash
# Backend packaging profiles -- TZ v4.2 §26.4.
#
# §26.4 gives one implementation two ways to be delivered and puts the choice in a build
# profile:
#
#     Способ поставки выбирается deployment profile через SVE_BACKEND_PACKAGING: STATIC,
#     DYNAMIC или BOTH. Packaging не меняет публичный encoder API, backend identifier,
#     capability semantics или backend selection.
#
# So the three profiles have to configure, build, and differ in the one way they are
# supposed to differ -- which of the shippable components the deployment links and
# registers, and whether the shared modules exist beside them. That is what this checks.
#
# The export list is checked because §26.4 says a plugin exports one C ABI entry point, and
# on the first build of these modules that was not what came out: hidden visibility governs
# what this project compiles, while libx264 and libx265 arrive from static archives already
# default-visible, so the x265 module exported 1318 symbols. A version script fixed it, and
# this is what keeps it fixed.
#
# What is deliberately not here yet: loading a plugin and encoding with it. There is no
# loader until the next slice, and a check that pretended otherwise would be measuring the
# build system twice.
#
#   ./integration/scripts/packaging-check.sh [build-dir]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-${ROOT}/build-packaging}"
fails=0
checked=0
pass() { printf '  ok    %s\n' "$1"; checked=$((checked + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); checked=$((checked + 1)); }

TOOLCHAIN=()
if [[ -f /mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake ]]; then
    TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE=/mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake)
fi

# §5.5's graph and nothing else: the two direct codec components are the ones shipped both
# ways, and everything that would drag libavcodec in is off. That is not thrift -- vcpkg
# builds FFmpeg static without -fPIC here, so a module linking it could not be built at all.
configure_profile() { # profile dir
    cmake -S "$ROOT" -B "$2" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_MODE=OFF "${TOOLCHAIN[@]}" \
        -DSVE_BACKEND_PACKAGING="$1" \
        -DSVE_BUILD_BACKEND_FFMPEG=OFF -DSVE_BUILD_FFMPEG_INTEROP=OFF \
        -DSVE_BUILD_EXAMPLES=OFF -DSVE_BUILD_TESTS=OFF \
        -DSVE_BUILD_INTEGRATION=OFF -DSVE_BUILD_TOOLS=OFF \
        > "$2.log" 2>&1 \
        && cmake --build "$2" -j"$(nproc)" >> "$2.log" 2>&1
}

# One exported function, whatever it was linked from.
#
# The symbol list is read once into a variable and then matched with bash's own tests, and
# that is not tidiness. `nm ... | grep -q` under `set -o pipefail` reports failure whenever
# grep finds its match early enough to close the pipe: nm dies of SIGPIPE, returns 141, and
# pipefail hands that up as the pipeline's status. It bit this script on its first run --
# the x265 module, with 1461 symbols to print, "had no entry point" while the x264 module
# with 22 had one, and the difference was entirely in who won the race to the pipe.
check_exports() { # profile dir
    local dir="$1" so name exported count
    for name in x264 x265; do
        so="${dir}/libsve_backend_${name}.so"
        if [[ ! -f "$so" ]]; then
            fail "${name}: no plugin was built"
            continue
        fi
        # The version node itself is an absolute symbol, not something the module defines.
        exported=$(nm -D --defined-only "$so" | awk '$2 != "A" {print $3}')
        count=$(printf '%s' "$exported" | grep -c '^' )
        if [[ "$count" == "1" ]]; then
            pass "${name} plugin exports exactly one symbol"
        else
            fail "${name} plugin exports ${count} symbols, expected 1"
            printf '%s\n' "$exported" | head -5 | sed 's/^/        /'
        fi
        if [[ "$exported" == *sve_backend_plugin_v1* ]]; then
            pass "${name} plugin exports §26.4's entry point"
        else
            fail "${name} plugin has no sve_backend_plugin_v1"
        fi
        # The arrangement of §26.4 as this project builds it: the core is not carried
        # inside the module, so a plugin cannot end up with its own copy of the registry it
        # is registering into. Its core references bind to the host at load time.
        if [[ "$(ldd "$so")" == *libsve* ]]; then
            fail "${name} plugin links a copy of the core"
        else
            pass "${name} plugin leaves the core to the host"
        fi
    done
}

registers() { # dir backend -> 0 if the generated aggregator calls its registration
    grep -q "Register${2}Backend" "${1}/generated/deployment_backends.cpp"
}

rm -rf "${BUILD}"
mkdir -p "${BUILD}"

echo "== STATIC =="
if configure_profile STATIC "${BUILD}/static"; then
    pass "the STATIC profile configures and builds"
    if compgen -G "${BUILD}/static/libsve_backend_*.so" >/dev/null; then
        fail "STATIC built a plugin"
    else
        pass "STATIC builds no plugins"
    fi
    if registers "${BUILD}/static" X264 && registers "${BUILD}/static" X265; then
        pass "STATIC registers both components by linking them"
    else
        fail "STATIC does not register the components it linked"
    fi
else
    fail "the STATIC profile does not build"
fi

echo "== DYNAMIC =="
if configure_profile DYNAMIC "${BUILD}/dynamic"; then
    pass "the DYNAMIC profile configures and builds"
    check_exports "${BUILD}/dynamic"
    # The one way DYNAMIC has to differ from STATIC. If it registered them as well, the two
    # profiles would be the same build and the parity §26.4 asks CI for would compare a
    # thing with itself.
    if registers "${BUILD}/dynamic" X264 || registers "${BUILD}/dynamic" X265; then
        fail "DYNAMIC still registers a component statically"
    else
        pass "DYNAMIC leaves the components to the loader"
    fi
else
    fail "the DYNAMIC profile does not build"
fi

echo "== BOTH =="
if configure_profile BOTH "${BUILD}/both"; then
    pass "the BOTH profile configures and builds"
    check_exports "${BUILD}/both"
    if registers "${BUILD}/both" X264 && registers "${BUILD}/both" X265; then
        pass "BOTH registers statically and ships the plugins beside it"
    else
        fail "BOTH did not register the components statically"
    fi
else
    fail "the BOTH profile does not build"
fi

echo
if [[ $checked -lt 3 ]]; then
    echo "packaging check ran only ${checked} check(s): nothing was measured"
    exit 1
fi
if [[ $fails -eq 0 ]]; then
    echo "packaging profiles passed (${checked} checks)"
else
    echo "${fails} check(s) failed (${checked} checks)"
fi
exit $(( fails > 0 ))
