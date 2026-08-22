#!/usr/bin/env bash
# Rate-distortion curves -- TZ v2.0 §38.
#
# Five operating points, baseline against adaptive, for one scene. The specification asks
# for 0.25x / 0.5x / 1x / 2x / 4x of a nominal bitrate; this encoder runs at a constant
# quantizer, where bitrate is an outcome rather than an input. Six CRF steps are worth
# about a factor of two in rate, so the points are CRF 38/34/30/26/22 around a nominal of
# 30 -- and the bitrate each one actually produced is recorded rather than assumed, which
# is the honest way to plot a curve whose x axis we do not control.
#
# Usage: rd_curve.sh [scene] [encoder] [codec]
set -euo pipefail

# awk and printf format decimals by locale, and this writes a comma-separated file: under a
# locale that uses a decimal comma, "79.6" comes out as "79,6" and splits into two columns.
# The numbers look right on screen, which is what makes it worth pinning here rather than
# noticing later that a plot is nonsense.
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCENE="${1:-moving}"
ENCODER="${2:-cpu}"
CODEC="${3:-h264}"

BUILD="${BUILD:-${ROOT}/build/integration}"
SRC="${ROOT}/testdata/scenes/${SCENE}/source.mp4"
ROI="${ROOT}/testdata/scenes/${SCENE}/roi.json"
TRUTH="${ROOT}/testdata/scenes/${SCENE}/truth.json"
OUT="${ROOT}/testdata/eval"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for f in "$SRC" "$ROI" "$TRUTH"; do
    [[ -f "$f" ]] || { echo "missing $f -- run benchmarks/synthetic/make-scenes.py" >&2; exit 1; }
done
[[ -x "${BUILD}/roi_metrics" ]] || { echo "roi_metrics not built" >&2; exit 1; }

# The baseline is the same encoder with nothing to protect and nothing to punish, so the
# only difference along the curve is the region handling.
python3 - "$ROI" "${WORK}/flat.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["samples"] = []
d["background_qp_delta"] = 0
json.dump(d, open(sys.argv[2], "w"))
PY

mkdir -p "$OUT"
CSV="${OUT}/rd_${SCENE}_${ENCODER}_${CODEC}.csv"
echo "variant,crf,bytes,kbps,roi_psnr,bg_psnr,separation,roi_ssim,bg_ssim" > "$CSV"

metric() {  # file, key
    "${BUILD}/roi_metrics" "$SRC" "$1" "$TRUTH" rd 2>/dev/null \
        | grep -oE "$2=[-0-9.]+" | head -1 | cut -d= -f2
}

duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$SRC")

printf '%-9s %4s %10s %8s %9s %9s %11s\n' variant crf bytes kbps roi_psnr bg_psnr separation
for crf in 38 34 30 26 22; do
    for variant in baseline adaptive; do
        roi_file="$ROI"
        [[ "$variant" == baseline ]] && roi_file="${WORK}/flat.json"
        out="${WORK}/${variant}_${crf}.mp4"
        "${BUILD}/roi-transcode" --in "$SRC" --roi "$roi_file" --out "$out" \
            --encoder "$ENCODER" --codec "$CODEC" --crf "$crf" >/dev/null 2>&1

        bytes=$(stat -c %s "$out")
        kbps=$(awk -v b="$bytes" -v d="$duration" 'BEGIN{printf "%.1f", b*8/1000/d}')
        rp=$(metric "$out" roi_psnr); bp=$(metric "$out" bg_psnr)
        rs=$(metric "$out" roi_ssim); bs=$(metric "$out" bg_ssim)
        sep=$(awk -v a="$rp" -v b="$bp" 'BEGIN{printf "%.3f", a-b}')

        echo "${variant},${crf},${bytes},${kbps},${rp},${bp},${sep},${rs},${bs}" >> "$CSV"
        printf '%-9s %4s %10s %8s %9s %9s %11s\n' \
            "$variant" "$crf" "$bytes" "$kbps" "$rp" "$bp" "$sep"
    done
done

echo
echo "written: $CSV"
