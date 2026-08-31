#!/usr/bin/env bash
# Two backends, the same clip, the same assertions -- what replaces byte-identity.
#
# Until now every refactor was checked by encoding a clip with the old build and the new one
# and comparing md5. That works exactly as long as the encode path does not change, and it
# is about to: TZ v4.1 §5.4 replaces libavcodec with direct libx264 and libx265, so the
# bytes will differ for a legitimate reason and the old check will go quiet rather than
# green.
#
# What survives the change is what the library actually promises. The ROI mechanism has to
# lift the zone, crush the background, protect the edge and cost fewer bytes than flat
# quality -- and it has to do that on the new backend at least as well as on the old one.
# So this compares the two by those numbers instead of by their bytes.
#
# The measurements are taken exactly as check.sh takes them (same crops, same ssim filter,
# same second of video), because a comparison against numbers gathered a different way is
# not a comparison.
#
#   ./backend-compare.sh x264 x264direct                 # defaults: synth clip, H.264
#   CODEC=h265 ./backend-compare.sh x265 x265direct
#   MARGIN=0.002 ./backend-compare.sh x264 x264direct    # how much worse B may be
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD:-${ROOT}/build/integration}"
CODEC="${CODEC:-h264}"
# How much worse the new backend's SSIM may be before it counts as a regression. Not a
# threshold from §49 -- those govern acceptance, not this -- but a stated margin for a
# like-for-like comparison, small enough that a real loss of ROI does not hide under it.
MARGIN="${MARGIN:-0.001}"

A="${1:?usage: backend-compare.sh <backend-a> <backend-b> [clip] [roi.json]}"
B="${2:?usage: backend-compare.sh <backend-a> <backend-b> [clip] [roi.json]}"
SRC="${3:-${ROOT}/testdata/synth.mp4}"
ROI="${4:-${ROOT}/testdata/synth_roi.json}"
NOROI="${ROOT}/testdata/synth_noroi.json"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The plate and a background patch of the same size, from check.sh. Hardcoded there too,
# and to the same numbers: they describe the synthetic fixture, not a preference.
PX=448 PY=288 PW=384 PH=144
BGX=40 BGY=40

fails=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

ssim() { # file x y w h
    ffmpeg -hide_banner -ss 1.0 -i "$SRC" -ss 1.0 -i "$1" -filter_complex \
        "[0:v]crop=$4:$5:$2:$3[a];[1:v]crop=$4:$5:$2:$3[b];[a][b]ssim" \
        -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2
}

# Sets the four numbers for one encode. Named rather than echoed so the caller keeps them.
declare -A ROI_SSIM BG_SSIM RING_SSIM SIZE BACKEND
measure() { # key backend roi.json
    local key="$1" enc="$2" roi="$3"
    local out="${WORK}/${key}.mp4"
    if ! "${BUILD}/roi-transcode" --in "$SRC" --roi "$roi" --out "$out" \
        --encoder "$enc" --codec "$CODEC" --crf 28 --gop 1 \
        >"${WORK}/${key}.log" 2>&1; then
        fail "encode ${key} (${enc})"
        sed 's/^/      /' "${WORK}/${key}.log" >&2
        return 1
    fi
    SIZE[$key]=$(stat -c %s "$out")
    ROI_SSIM[$key]=$(ssim "$out" $PX $PY $PW $PH)
    BG_SSIM[$key]=$(ssim "$out" $BGX $BGY $PW $PH)
    RING_SSIM[$key]=$(ssim "$out" $((PX - 16)) $((PY - 16)) $((PW + 32)) $((PH + 32)))
    # Which backend actually opened, so a silent fallback cannot make this compare a
    # backend with itself and report agreement.
    BACKEND[$key]=$(grep -oE 'backend: [^ ]+' "${WORK}/${key}.log" | head -1 | cut -d' ' -f2)
}

gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }
not_worse() { awk -v a="$1" -v b="$2" -v m="$3" 'BEGIN { exit !(b >= a - m) }'; }

echo "== ${A} vs ${B}, ${CODEC}, $(basename "$SRC") =="

for side in "$A" "$B"; do
    measure "roi_${side}"   "$side" "$ROI"   || exit 1
    measure "noroi_${side}" "$side" "$NOROI" || exit 1
done

printf '\n  %-10s %-22s %10s %10s %10s %10s\n' \
    backend opened roi bg ring bytes
for side in "$A" "$B"; do
    printf '  %-10s %-22s %10s %10s %10s %10s\n' \
        "$side" "${BACKEND[roi_${side}]:-?}" "${ROI_SSIM[roi_${side}]}" \
        "${BG_SSIM[roi_${side}]}" "${RING_SSIM[roi_${side}]}" "${SIZE[roi_${side}]}"
done
echo

# What must hold for each of them on its own. A backend that fails these is not applying
# ROI at all, whatever it agrees with.
for side in "$A" "$B"; do
    gt "${ROI_SSIM[roi_${side}]}" "${BG_SSIM[roi_${side}]}" \
        && pass "[${side}] the zone is kept sharper than the background" \
        || fail "[${side}] the zone is kept sharper than the background (roi=${ROI_SSIM[roi_${side}]} bg=${BG_SSIM[roi_${side}]})"
    awk -v a="${SIZE[roi_${side}]}" -v b="${SIZE[noroi_${side}]}" 'BEGIN { exit !(a < b) }' \
        && pass "[${side}] ROI costs less than flat quality" \
        || fail "[${side}] ROI costs less than flat quality (${SIZE[roi_${side}]} vs ${SIZE[noroi_${side}]})"
done

# And what must hold between them: the new path may encode differently, but not worse.
not_worse "${ROI_SSIM[roi_${A}]}" "${ROI_SSIM[roi_${B}]}" "$MARGIN" \
    && pass "[${B}] keeps the zone as well as ${A} (margin ${MARGIN})" \
    || fail "[${B}] loses the zone against ${A}: ${ROI_SSIM[roi_${B}]} < ${ROI_SSIM[roi_${A}]} - ${MARGIN}"
not_worse "${RING_SSIM[roi_${A}]}" "${RING_SSIM[roi_${B}]}" "$MARGIN" \
    && pass "[${B}] protects the edge as well as ${A} (margin ${MARGIN})" \
    || fail "[${B}] loses the edge against ${A}: ${RING_SSIM[roi_${B}]} < ${RING_SSIM[roi_${A}]} - ${MARGIN}"

# The two must not be the same backend wearing two names -- otherwise every line above
# passes and proves nothing.
if [[ "${BACKEND[roi_${A}]:-a}" == "${BACKEND[roi_${B}]:-b}" ]]; then
    fail "both names opened the same backend (${BACKEND[roi_${A}]:-?}): this compared nothing"
fi

echo
if (( fails > 0 )); then
    echo "${fails} check(s) failed"
    exit 1
fi
echo "all checks passed"
