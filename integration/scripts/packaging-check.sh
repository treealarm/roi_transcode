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
# The linkage is checked as well as the exports, and it is the check that earned its place:
# the first arrangement tried here left the core out of the module and bound its references
# to the host at load time, and the parity section below found that a host linking the core
# from a static archive exports only the parts it happens to use. Every plugin was refused
# with "undefined symbol" and the deployment had no backends.
#
# The export list is checked because §26.4 says a plugin exports one C ABI entry point, and
# on the first build of these modules that was not what came out: hidden visibility governs
# what this project compiles, while libx264 and libx265 arrive from static archives already
# default-visible, so the x265 module exported 1318 symbols. A version script fixed it, and
# this is what keeps it fixed.
#
# The last section is §26.4's parity clause -- "parity capability/selection/functional
# результатов static и dynamic вариантов при одинаковых версиях зависимостей и build
# options". Two full builds differing in one option, and three things that must not differ:
# what the capability report says, which backend a name-free selection picks, and the bytes
# that come out. It opens with the negative, because parity between two builds that are
# both secretly static would pass every comparison and prove nothing.
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
        # The arrangement of §26.4 as this project builds it: the plugin links the core,
        # and in these profiles the core is shared, so the process holds one copy of it. A
        # module carrying its own would carry its own default_backend_registry() with it,
        # and register into a registry nobody asks. Leaving the reference undefined and
        # binding it to the host was tried first and does not hold -- see the note in
        # CMakeLists.txt beside SVE_BACKEND_PACKAGING.
        if [[ "$(ldd "$so")" == *libsve.so* ]]; then
            pass "${name} plugin links the one shared core"
        else
            fail "${name} plugin does not link the shared core"
            ldd "$so" | sed 's/^/        /' | head -5
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

# --- §26.4 parity ----------------------------------------------------------
# Full builds this time: the comparison needs the tool that reports capabilities and the
# program that encodes, which the sections above do not.
configure_full() { # profile dir
    cmake -S "$ROOT" -B "$2" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_MODE=OFF "${TOOLCHAIN[@]}" \
        -DSVE_BACKEND_PACKAGING="$1" \
        -DSVE_BUILD_BACKEND_FFMPEG=OFF -DSVE_BUILD_FFMPEG_INTEROP=ON \
        -DSVE_BUILD_EXAMPLES=ON -DSVE_BUILD_TESTS=OFF \
        -DSVE_BUILD_INTEGRATION=ON -DSVE_BUILD_TOOLS=ON \
        > "$2.log" 2>&1 \
        && cmake --build "$2" -j"$(nproc)" >> "$2.log" 2>&1
}

echo "== parity: STATIC against DYNAMIC =="
CLIP="${ROOT}/testdata/synth.mp4"
ROI="${ROOT}/testdata/synth_roi.json"
if [[ ! -f "$CLIP" || ! -f "$ROI" ]]; then
    echo "  skip  parity needs testdata/synth.mp4 (integration/scripts/make-synth.sh)"
elif configure_full STATIC "${BUILD}/full-static" \
     && configure_full DYNAMIC "${BUILD}/full-dynamic"; then
    pass "both full profiles configure and build"
    ST="${BUILD}/full-static"
    DY="${BUILD}/full-dynamic"

    # The negative, and the reason the rest means anything. With the libavcodec group off,
    # a DYNAMIC build registers nothing at all by linking: if it can still encode without
    # being handed a plugin directory, then it is not a dynamic deployment and every
    # comparison below is a build compared with itself.
    if "${DY}/integration/roi-transcode" --in "$CLIP" --roi "$ROI" \
        --out "${BUILD}/should-not-exist.mp4" --encoder x264direct --codec h264 \
        > "${BUILD}/dynamic-noplugin.log" 2>&1; then
        fail "the DYNAMIC build encoded without being given a plugin directory"
    else
        pass "the DYNAMIC build has no backend until a plugin is loaded"
    fi

    # §26.4 parity, capability. Compared on stdout: refusals go to stderr precisely so that
    # a report can be diffed.
    "${ST}/sve-capabilities" > "${BUILD}/caps-static.txt" 2>/dev/null
    "${DY}/sve-capabilities" --plugin-dir "${DY}" > "${BUILD}/caps-dynamic.txt" 2>/dev/null
    if [[ ! -s "${BUILD}/caps-static.txt" ]]; then
        fail "the STATIC capability report is empty"
    elif diff -q "${BUILD}/caps-static.txt" "${BUILD}/caps-dynamic.txt" >/dev/null; then
        pass "capability reports are identical across packaging"
    else
        fail "capability reports differ across packaging"
        diff "${BUILD}/caps-static.txt" "${BUILD}/caps-dynamic.txt" | sed 's/^/        /' | head -10
    fi

    # §26.4 parity, selection. Backend::Auto with no backend named: the same chain has to
    # end at the same backend whichever way it arrived.
    "${ST}/integration/roi-transcode" --in "$CLIP" --roi "$ROI" \
        --out "${BUILD}/parity-static.mp4" --encoder auto --codec h264 --crf 32 --gop 1 \
        > "${BUILD}/parity-static.log" 2>&1
    "${DY}/integration/roi-transcode" --in "$CLIP" --roi "$ROI" \
        --out "${BUILD}/parity-dynamic.mp4" --encoder auto --codec h264 --crf 32 --gop 1 \
        --plugin-dir "${DY}" > "${BUILD}/parity-dynamic.log" 2>&1
    st_pick=$(grep -m1 '^encoded with' "${BUILD}/parity-static.log")
    dy_pick=$(grep -m1 '^encoded with' "${BUILD}/parity-dynamic.log")
    if [[ -n "$st_pick" && "$st_pick" == "$dy_pick" ]]; then
        pass "backend selection is identical across packaging"
    else
        fail "selection differs: static '${st_pick}' against dynamic '${dy_pick}'"
    fi

    # §26.4 parity, functional. Same sources compiled the same way, so anything but
    # byte-identical output means packaging changed an encoding decision, which §26.4
    # forbids in as many words.
    if [[ -s "${BUILD}/parity-static.mp4" ]] \
        && cmp -s "${BUILD}/parity-static.mp4" "${BUILD}/parity-dynamic.mp4"; then
        pass "the encoded output is byte-identical across packaging"
    else
        fail "the encoded output differs across packaging"
    fi
else
    fail "the full STATIC/DYNAMIC pair does not build"
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
