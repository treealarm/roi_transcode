#!/usr/bin/env bash
# Measures what the halo around a box is worth, and how strong the box itself
# should be. pad_norm and pad_qp_delta are guesses in roi_map_builder.cpp; this
# is the experiment that replaces them with numbers.
#
# Runs on Quick Sync deliberately: it is CQP, so the QP offsets are independent
# per block and the background stays fixed while the zone varies. Under x265's
# CRF the rate control would couple them and every row would move at once.
set -euo pipefail

# awk and printf follow the locale: under a comma-decimal locale a formatted
# number comes out as "0,02", which is not a number to JSON and not comparable
# as one here either.
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
SRC="${ROOT}/testdata/synth.mp4"
OUT="${ROOT}/testdata/eval/sweep"
CSV="${OUT}/padding.csv"
W=1280; H=720
PX=448; PY=288; PW=384; PH=144
BGX=40; BGY=40
# A ring just outside the box, where a QP step shows up as ringing.
RX=$((PX - 16)); RY=$((PY - 16)); RW=$((PW + 32)); RH=$((PH + 32))

# shellcheck source=/dev/null
source "${ROOT}/scripts/qsv-env.sh"
mkdir -p "$OUT"
echo "pad_px,qp_delta,bytes,ssim_plate,ssim_ring,ssim_bg" > "$CSV"

ssim() {
    ffmpeg -hide_banner -ss 1.0 -i "$SRC" -ss 1.0 -i "$1" -filter_complex \
        "[0:v]crop=$4:$5:$2:$3[a];[1:v]crop=$4:$5:$2:$3[b];[a][b]ssim" \
        -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2
}

for pad_px in 0 16 32 48 64 80 96 128; do
    for qp in -4 -8 -12; do
        pad_norm=$(awk "BEGIN{printf \"%.6f\", ${pad_px}/${W}}")
        json="${OUT}/roi_p${pad_px}_q${qp#-}.json"
        cat > "$json" <<JSON
{
  "background_qp_delta": 20,
  "hold_sec": 20,
  "kinds": { "plate": { "qp_delta": ${qp}, "pad_norm": ${pad_norm} } },
  "samples": [ { "t": 0, "kind": "plate",
                 "bbox_x": 0.35, "bbox_y": 0.4, "bbox_w": 0.3, "bbox_h": 0.2 } ]
}
JSON
        out="${OUT}/p${pad_px}_q${qp#-}.mp4"
        "${BUILD}/roi-transcode" --in "$SRC" --roi "$json" --out "$out" \
            --encoder qsv --crf 28 --gop 1 >/dev/null 2>&1
        printf '%s,%s,%s,%s,%s,%s\n' "$pad_px" "$qp" "$(stat -c %s "$out")" \
            "$(ssim "$out" $PX $PY $PW $PH)" \
            "$(ssim "$out" $RX $RY $RW $RH)" \
            "$(ssim "$out" $BGX $BGY $PW $PH)" >> "$CSV"
    done
done

echo "wrote $CSV"
column -t -s, "$CSV"
