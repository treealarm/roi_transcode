#!/usr/bin/env bash
# One clip, one box, many settings: bytes, quality inside and outside the zone, and the
# wall clock it cost -- TZ v4.2 §41's per-configuration measurement, on real footage.
#
#     real-clip-sweep.sh CLIP [OUT_DIR] -- VARIANT [VARIANT...]
#
# A VARIANT is a label followed by the roi-transcode arguments it stands for, joined by
# colons, e.g. "preset-fast:--preset:fast". The label names the row; everything after it is
# appended to the common arguments.
#
# Why a script rather than a session's worth of ad-hoc commands: the first real-footage
# measurements (docs/STATUS.md, срез Z14) were re-run three times because the method lived
# in a terminal. Both mistakes are structural here rather than remembered --
#
#   * Orientation. These clips carry rotation=-180. Comparing an encode against a reference
#     that rotates differently measures the rotation, which is what produced "ROI makes the
#     zone worse" and cost an evening. Safe since срез Z15: roi-transcode carries the
#     display matrix to the output, so reference and encode rotate alike. It stops being
#     safe the moment something drops it again, which is why it is written down here.
#   * No -ss anywhere, deliberately. Seeking before -i lands on a keyframe, and files with
#     different GOPs have different ones, so two encodes get compared at different frames.
#     Every variant below encodes the whole clip.
#
# Needs: build/integration/roi-transcode, build/integration/roi_metrics, ffprobe.
set -euo pipefail

CLIP=${1:?usage: real-clip-sweep.sh CLIP [OUT_DIR] -- LABEL:ARG:ARG ...}
shift
OUT=/tmp/real-clip-sweep
if [[ ${1:-} != "--" ]]; then
    OUT=$1
    shift
fi
[[ ${1:-} == "--" ]] || { echo "expected -- before the variants" >&2; exit 2; }
shift

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="${ROI_TRANSCODE_BIN:-${ROOT}/build/integration/roi-transcode}"
METRICS="${ROI_METRICS_BIN:-${ROOT}/build/integration/roi_metrics}"
for tool in "$BIN" "$METRICS"; do
    [[ -x "$tool" ]] || { echo "missing $tool" >&2; exit 1; }
done

CLIP=$(readlink -f "$CLIP")
mkdir -p "$OUT"

# Geometry read from the file rather than assumed: the box below is in normalised
# coordinates and the truth the metrics need is in pixels.
read -r WIDTH HEIGHT FPS < <(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$CLIP" \
    | awk -F, '{split($3,r,"/"); print $1, $2, r[1]/r[2]}')
# nw=1:nk=1 rather than csv: csv appends a trailing comma to a single field, and the
# comma reaches python as part of the number.
FRAMES=$(ffprobe -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$CLIP")

# Boxes held still, and fixed rather than detected: this repository has no detector, and a
# box that moved would make two variants differ by where it was as well as by the setting
# under test (docs/tuning.md, раздел 14).
SWEEP_BOXES=${SWEEP_BOXES:-plate}
# The delta the zone is bought with. Overridable because it decides whether anything else
# in the sweep can act at all: measured 2026-09-04, +8 on this material moves the file by
# 0.1% while +25 moves it by 54%, so a knob swept at +8 would look inert whatever it does.
BACKGROUND_QP_DELTA=${SWEEP_BG:-8}

python3 - "$OUT" "$WIDTH" "$HEIGHT" "$FPS" "$FRAMES" "$SWEEP_BOXES" \
    "$BACKGROUND_QP_DELTA" <<'PY'
import json, sys
out, w, h, fps, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), \
    float(sys.argv[4]), int(sys.argv[5])
which, delta = sys.argv[6], int(sys.argv[7])

# x, y, width, height in normalised coordinates, and the kind the importance ladder weighs.
SETS = {
    # One plate in the middle: 2.4% of the frame, well under any protected-area budget.
    "plate": [(0.40, 0.44, 0.20, 0.12, "plate")],
    # Three vehicles at 9% each. 27% together, which is what puts them over the default
    # budget of a quarter and makes the last one fall outside it.
    "vehicles": [(0.05, 0.10, 0.30, 0.30, "vehicle"),
                 (0.35, 0.40, 0.30, 0.30, "vehicle"),
                 (0.65, 0.65, 0.30, 0.30, "vehicle")],
    # The same geometry under a kind the ladder of §11 values at 1.00 instead of 0.60.
    # Same area, same budget arithmetic, and the only thing that differs is how much the
    # protection is worth -- which is what tells a budget that binds from one that matters.
    "plates-large": [(0.05, 0.10, 0.30, 0.30, "plate"),
                     (0.35, 0.40, 0.30, 0.30, "plate"),
                     (0.65, 0.65, 0.30, 0.30, "plate")],
    # One box over 49% of the frame -- the case §12's budget exists for. Its own comment
    # names it: a person box averages 41.5% of the frame, and protecting that much makes
    # the file grow instead of shrink, because there is not enough background left to pay.
    "one-huge": [(0.15, 0.15, 0.70, 0.70, "person")],
}
boxes = SETS[which]

objects = []
samples = []
for i, (bx, by, bw, bh, kind) in enumerate(boxes, start=1):
    objects.append({"id": i, "class": kind,
                    "bbox": [int(round(bx * w)), int(round(by * h)),
                             max(1, int(round(bw * w))), max(1, int(round(bh * h)))]})
    samples.append({"kind": kind, "x": bx, "y": by, "width": bw, "height": bh,
                    "track_id": i, "confidence": 0.95})

truth = {"scene": "real", "width": w, "height": h, "fps": fps, "frame_count": frames,
         "frames": [{"index": i, "t": i / fps, "objects": objects}
                    for i in range(frames)]}
json.dump(truth, open(f"{out}/truth.json", "w"))

# One sample at t=0: the boxes do not move, and a sample per frame would only be a larger
# file saying the same thing.
json.dump({"background_qp_delta": delta, "pad": 0.02,
           "samples": [{"t_sec": 0.0, "boxes": samples}]},
          open(f"{out}/roi.json", "w"))
PY

# Common to every variant, so that a row differs from its neighbours only in what it names.
COMMON=(--encoder x264direct --codec h264 --crf 28 --gop 60)

CSV="$OUT/sweep.csv"
echo "label,bytes,bitrate_bps,roi_ssim,bg_ssim,roi_psnr,bg_psnr,encode_sec" >"$CSV"

DURATION=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$CLIP")
echo "clip: $(basename "$CLIP")  ${WIDTH}x${HEIGHT}  ${FPS} fps  ${FRAMES} frames  ${DURATION}s"
echo "boxes: ${SWEEP_BOXES}, background +${BACKGROUND_QP_DELTA}"
echo

for variant in "$@"; do
    IFS=: read -r label extra <<<"$variant"
    IFS=: read -r -a parts <<<"${extra:-}"
    dest="$OUT/${label}.mp4"

    # Split what the variant carries: flags go to the encoder, @field=value goes into a ROI
    # document of this variant's own, so that no two variants ever share an edited one.
    args=()
    roi="$OUT/roi.json"
    for part in ${parts[@]+"${parts[@]}"}; do
        if [[ $part == @* ]]; then
            # Chained, not restarted: each override reads whatever this variant has built so
            # far and writes it back. Reading the pristine base every time meant a second
            # @field=value silently discarded the first, and the CSV row then measured
            # something other than what its label said.
            base=$roi
            roi="$OUT/roi-${label}.json"
            python3 - "$base" "$roi" "${part#@}" <<'PY'
import json, sys
base, dest, assignment = sys.argv[1:4]
field, _, value = assignment.partition("=")
doc = json.load(open(base))
# Integer fields stay integers. background_qp_delta written as 20.0 is not the same
# document as 20, and whether the reader coerces it is not something to find out by
# accident in the middle of a measurement.
try:
    doc[field] = int(value)
except ValueError:
    doc[field] = float(value)
json.dump(doc, open(dest, "w"))
PY
        else
            args+=("$part")
        fi
    done

    start=$(date +%s.%N)
    "$BIN" --in "$CLIP" --roi "$roi" --out "$dest" \
        "${COMMON[@]}" ${args[@]+"${args[@]}"} >"$OUT/${label}.log" 2>&1
    finish=$(date +%s.%N)
    elapsed=$(python3 -c "print(f'{$finish - $start:.1f}')")

    bytes=$(wc -c <"$dest" | tr -d ' ')
    line=$("$METRICS" "$CLIP" "$dest" "$OUT/truth.json" "$label" 2>/dev/null)
    value() { grep -oE "$1=[-0-9.]+" <<<"$line" | head -1 | cut -d= -f2; }
    bps=$(python3 -c "print(f'{8*$bytes/$DURATION:.0f}')")

    echo "${label},${bytes},${bps},$(value roi_ssim),$(value bg_ssim),$(value roi_psnr),$(value bg_psnr),${elapsed}" >>"$CSV"
    printf '  %-18s %10s bytes  %8s bps  roi_ssim=%-8s bg_ssim=%-8s  %ss\n' \
        "$label" "$bytes" "$bps" "$(value roi_ssim)" "$(value bg_ssim)" "$elapsed"
done

echo
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
echo "written: $CSV"
