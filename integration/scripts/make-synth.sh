#!/usr/bin/env bash
# Synthetic plate clip + QSV encode with/without ROI. Writes testdata/eval/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=/dev/null
source "${ROOT}/integration/scripts/qsv-env.sh"

W=1280
H=720
FPS=25
DUR=12
# Must match testdata/synth_roi.json (normalized).
PX=$(python3 -c "print(int(round(0.35*$W)))")
PY=$(python3 -c "print(int(round(0.4*$H)))")
PW=$(python3 -c "print(int(round(0.3*$W)))")
PH=$(python3 -c "print(int(round(0.2*$H)))")
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
EVAL="${ROOT}/testdata/eval"
SRC="${ROOT}/testdata/synth.mp4"
BIN="${ROOT}/build/integration/roi-transcode"
mkdir -p "$EVAL"

echo "plate box ${PX},${PY} ${PW}x${PH} on ${W}x${H}, ${DUR}s static grid"

BG_PNG="${EVAL}/bg_still.png"
PL_PNG="${EVAL}/pl_still.png"

# One still each, then loop — background must not crawl.
# Hard lines every 32px (not 16): dense 16px grid at QP 48 made P-frames
# refuse to skip, so ROI files grew even though I-frames shrank.
ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=0x3a3a3a:s=${W}x${H}:d=1" \
  -vf "format=yuv420p,\
geq=lum='if(lt(mod(X\,32)\,3)\,235\,if(lt(mod(Y\,32)\,3)\,235\,70))':cb=128:cr=128,\
drawgrid=width=128:height=128:thickness=3:color=0xffff00@0.95" \
  -frames:v 1 "$BG_PNG"

ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=0xe8e8e8:s=${PW}x${PH}:d=1" \
  -frames:v 1 "$PL_PNG"

ffmpeg -y -hide_banner -loglevel error \
  -loop 1 -framerate "${FPS}" -t "${DUR}" -i "$BG_PNG" \
  -loop 1 -framerate "${FPS}" -t "${DUR}" -i "$PL_PNG" \
  -filter_complex "\
[0:v][1:v]overlay=${PX}:${PY},\
drawbox=x=${PX}:y=${PY}:w=${PW}:h=${PH}:color=black:t=8,\
drawtext=fontfile=${FONT}:text='A123BC 77':fontsize=48:fontcolor=black:x=${PX}+(${PW}-text_w)/2:y=${PY}+(${PH}-text_h)/2-10,\
drawtext=fontfile=${FONT}:text='RUS 12 345 678':fontsize=16:fontcolor=black:x=${PX}+(${PW}-text_w)/2:y=${PY}+${PH}-36" \
  -c:v libx264 -preset fast -crf 16 -pix_fmt yuv420p -g 50 "$SRC"

# Base QP 28. --gop 1 (all intra): P-frames on this static grid "repair"
# the crushed background and make ROI *larger*. I-frames show the real save.
QP=28
"$BIN" --in "$SRC" --roi "${ROOT}/testdata/synth_noroi.json" \
  --out "${EVAL}/noroi.mp4" --encoder qsv --crf "$QP" --gop 1
"$BIN" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
  --out "${EVAL}/roi.mp4" --encoder qsv --crf "$QP" --gop 1

grab() {
  local in=$1 out=$2
  ffmpeg -y -hide_banner -loglevel error -ss 1.0 -i "$in" -frames:v 1 -q:v 2 "$out"
}

grab "$SRC" "${EVAL}/src.png"
grab "${EVAL}/noroi.mp4" "${EVAL}/noroi.png"
grab "${EVAL}/roi.mp4" "${EVAL}/roi.png"

# Plate crops
for name in src noroi roi; do
  ffmpeg -y -hide_banner -loglevel error -i "${EVAL}/${name}.png" \
    -vf "crop=${PW}:${PH}:${PX}:${PY}" "${EVAL}/plate_${name}.png"
done
# Busy background patch (top-left), same size as plate
BGX=40
BGY=40
for name in src noroi roi; do
  ffmpeg -y -hide_banner -loglevel error -i "${EVAL}/${name}.png" \
    -vf "crop=${PW}:${PH}:${BGX}:${BGY}" "${EVAL}/bg_${name}.png"
done

ffmpeg -y -hide_banner -loglevel error \
  -i "${EVAL}/plate_noroi.png" -i "${EVAL}/plate_roi.png" \
  -filter_complex "hstack=inputs=2,drawtext=fontfile=${FONT}:text='no ROI | ROI':fontsize=28:fontcolor=yellow:x=12:y=8" \
  "${EVAL}/plate_compare.png"
ffmpeg -y -hide_banner -loglevel error \
  -i "${EVAL}/bg_src.png" -i "${EVAL}/bg_noroi.png" -i "${EVAL}/bg_roi.png" \
  -filter_complex "hstack=inputs=3,drawtext=fontfile=${FONT}:text='src | no ROI | ROI  (background)':fontsize=26:fontcolor=yellow:x=12:y=8" \
  "${EVAL}/bg_compare.png"

echo
echo "sizes:"
stat -c '%s %n' "$SRC" "${EVAL}/noroi.mp4" "${EVAL}/roi.mp4"

echo
echo "SSIM vs source (higher = closer to original):"
ssim_pair() {
  local label=$1 enc=$2 x=$3 y=$4 w=$5 h=$6
  ffmpeg -hide_banner -ss 1.0 -i "$SRC" -ss 1.0 -i "$enc" -filter_complex \
    "[0:v]crop=${w}:${h}:${x}:${y}[a];[1:v]crop=${w}:${h}:${x}:${y}[b];[a][b]ssim" \
    -f null - 2>&1 | grep -oE 'SSIM.+$' | sed "s/^/${label}: /"
}
ssim_pair "plate" "${EVAL}/noroi.mp4" "$PX" "$PY" "$PW" "$PH"
ssim_pair "plate" "${EVAL}/roi.mp4" "$PX" "$PY" "$PW" "$PH"
ssim_pair "bg   " "${EVAL}/noroi.mp4" "$BGX" "$BGY" "$PW" "$PH"
ssim_pair "bg   " "${EVAL}/roi.mp4" "$BGX" "$BGY" "$PW" "$PH"
echo "wrote ${EVAL}/plate_compare.png ${EVAL}/bg_compare.png"
