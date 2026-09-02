#!/usr/bin/env bash
# Baseline calibration -- TZ v4.2 §38, and the "baseline profiles versioned" line of §55.
#
# §38 asks that a baseline manifest be created for each combination of backend, codec,
# hardware class, resolution, dataset and operating point before final acceptance, and that
# changing any field create a new profile version. The manifests live in baselines/ and are
# committed: that is what "versioned" means here -- a profile nobody can diff against is a
# number in a log, not a baseline.
#
# Two modes:
#
#     ./integration/scripts/baseline-check.sh [build-dir]      # verify, the acceptance one
#     BASELINE_UPDATE=1 ./integration/scripts/baseline-check.sh # regenerate and write
#
# Verify regenerates every profile and compares it to the committed one. A difference is a
# failure and names the drifted lines, because that is exactly the event §38 is about: the
# configuration this project encodes with has changed, and either the change was intended --
# in which case the new profile gets committed and carries a new profile_version -- or it
# was not, and the diff is the first anyone would have heard of it.
#
# Scope, stated rather than implied: only the direct software backends are calibrated. §38
# wants the exact runtime version of the encoder, and libx264 and libx265 are linked
# statically so the build knows theirs. A backend whose encoder lives in a driver has a
# version this build cannot see; sve-baseline refuses those with exit code 3 and the refusal
# is counted as a skip, not passed over.
#
# The committed profiles are pinned to this calibration environment, libx264 and libx265
# versions included. On a machine with different ones the comparison fails by design: that
# is a different profile version in §38's terms, not a false alarm.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-${ROOT}/build}"
TOOL="${BUILD}/sve-baseline"
BASELINES="${ROOT}/baselines"

# The software backends run on the CPU, and the architecture is what distinguishes one
# class of them from another as far as an encoder is concerned. §38 names the dimension;
# it does not say how to name a class.
HW_CLASS="cpu-$(uname -m)"

# The one dataset that exists here. MOT17 and UFPR-ALPR are blocked on not having them --
# docs/STATUS.md -- and a synthetic 720p30 clip is what §39's first row is defined for.
DATASET="synthetic"
WIDTH=1280
HEIGHT=720
FPS=30
RES_CLASS="720p30"

# backend:codec. x264 produces H.264 and x265 produces H.265; the pair that does not exist
# is not a gap to report, it is a backend that never claimed to.
COMBINATIONS=("x264direct:h264" "x265direct:h265")

fails=0
oks=0
skips=0
pass() { printf '  ok    %s\n' "$1"; oks=$((oks + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
skip() { printf '  skip  %s\n' "$1"; skips=$((skips + 1)); }

if [[ ! -x "$TOOL" ]]; then
    echo "no sve-baseline at ${TOOL}; build with -DSVE_BUILD_TOOLS=ON" >&2
    exit 1
fi

update="${BASELINE_UPDATE:-}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

mkdir -p "${BASELINES}/${HW_CLASS}"

echo "== §38 baseline calibration (${HW_CLASS}, ${DATASET}, ${RES_CLASS}) =="
for combo in "${COMBINATIONS[@]}"; do
    backend="${combo%%:*}"
    codec="${combo##*:}"
    for point in R1 R2 R3 R4; do
        name="${backend}-${codec}-${RES_CLASS}-${DATASET}-${point}.yaml"
        committed="${BASELINES}/${HW_CLASS}/${name}"
        fresh="${work}/${name}"

        "$TOOL" --hardware-class "$HW_CLASS" --dataset "$DATASET" \
            --backend "$backend" --codec "$codec" \
            --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" \
            --point "$point" >"$fresh" 2>"${fresh}.log"
        rc=$?

        case "$rc" in
        0) ;;
        3) skip "${name}: this build cannot version ${backend}'s encoder"; continue ;;
        4) skip "${name}: ${backend} cannot open on this machine"; continue ;;
        *) fail "${name}: sve-baseline exited ${rc}"
           sed 's/^/      /' "${fresh}.log" >&2
           continue ;;
        esac

        if [[ -n "$update" ]]; then
            cp "$fresh" "$committed"
            pass "${name}: written"
            continue
        fi

        if [[ ! -f "$committed" ]]; then
            fail "${name}: no committed profile; run with BASELINE_UPDATE=1 and commit it"
            continue
        fi
        if diff -u "$committed" "$fresh" >"${fresh}.diff"; then
            pass "${name}: matches the committed profile"
        else
            fail "${name}: the configuration drifted from the committed profile"
            sed 's/^/      /' "${fresh}.diff" >&2
        fi
    done
done

# A profile that no combination produces any more would sit in git for ever, still passing
# because nothing regenerates it. Named here so that deleting a backend is visible.
if [[ -z "$update" ]]; then
    for committed in "${BASELINES}/${HW_CLASS}"/*.yaml; do
        [[ -e "$committed" ]] || continue
        if [[ ! -e "${work}/$(basename "$committed")" ]]; then
            fail "$(basename "$committed"): committed but nothing regenerates it"
        fi
    done
fi

printf '\n'
if [[ "$fails" -gt 0 ]]; then
    echo "baseline calibration FAILED: ${fails} failed, ${oks} ok, ${skips} skipped"
    exit 1
fi
echo "baseline calibration passed: ${oks} ok, ${skips} skipped"
