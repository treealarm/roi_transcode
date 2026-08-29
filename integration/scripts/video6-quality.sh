#!/usr/bin/env bash
# PSNR/SSIM on video6: adaptive (ROI) vs baseline (no regions) at matched settings.
#
# Usage: video6-quality.sh [input.mp4] [boxes.json] [out_dir]
#
# Defaults: seenaptec video6 + /tmp/roi-metrics-v6/video6.boxes.json
set -euo pipefail

INPUT=${1:-/mnt/wdblue/seenaptec/media_trash/samples/dataset/video6.mp4}
BOXES=${2:-/tmp/roi-metrics-v6/video6.boxes.json}
OUT=${3:-/tmp/video6-quality}

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="${ROI_TRANSCODE_BIN:-${ROOT}/build/integration/roi-transcode}"
METRICS="${ROI_METRICS_BIN:-${ROOT}/build/integration/roi_metrics}"

mkdir -p "$OUT"
INPUT=$(readlink -f "$INPUT")
BOXES=$(readlink -f "$BOXES")

for tool in "$BIN" "$METRICS"; do
  [[ -x "$tool" ]] || { echo "missing $tool" >&2; exit 1; }
done

TRUTH="$OUT/truth.json"
SAMPLES="$OUT/adaptive.samples.json"
BASE_JSON="$OUT/baseline_empty.json"

python3 - "$BOXES" "$TRUTH" "$SAMPLES" <<'PY'
import json, sys
boxes_path, truth_path, samples_path = sys.argv[1:4]
doc = json.load(open(boxes_path))
W = int(doc.get("width") or 1280)
H = int(doc.get("height") or 720)
fps = float(doc.get("fps") or 30)

truth_frames = []
samples = []
for i, fr in enumerate(doc.get("frames") or []):
    t = fr.get("t", 0)
    t_sec = float(t) / fps if isinstance(t, int) and t > 10 else float(t)
    objects = []
    sample_boxes = []
    for b in fr.get("boxes") or []:
        x = int(round(float(b["x"]) * W))
        y = int(round(float(b["y"]) * H))
        w = max(1, int(round(float(b.get("w", b.get("width", 0))) * W)))
        h = max(1, int(round(float(b.get("h", b.get("height", 0))) * H)))
        objects.append({
            "id": int(b.get("track_id") or 0),
            "class": b.get("kind", ""),
            "bbox": [x, y, w, h],
        })
        sample_boxes.append({
            "kind": b.get("kind", ""),
            "x": b["x"], "y": b["y"],
            "width": b.get("w", b.get("width")),
            "height": b.get("h", b.get("height")),
            "track_id": int(b.get("track_id") or 0),
            "confidence": float(b.get("confidence") or 0),
        })
    truth_frames.append({"index": i, "t": t_sec, "objects": objects})
    samples.append({"t_sec": t_sec, "boxes": sample_boxes})

json.dump({
    "scene": "video6",
    "width": W, "height": H, "fps": fps,
    "frame_count": len(truth_frames),
    "frames": truth_frames,
}, open(truth_path, "w"), indent=1)

json.dump({
    "background_qp_delta": 14,
    "pad": 0.02,
    "samples": samples,
}, open(samples_path, "w"))
print(f"truth: {len(truth_frames)} frames, {W}x{H}", file=sys.stderr)
PY

printf '%s\n' '{"background_qp_delta":14,"pad":0.02,"samples":[]}' >"$BASE_JSON"

# Product stand defaults (roi-clip): crush background, ladder picks vehicle → Normal.
COMMON=(--encoder cpu --codec h264 --crf 36 --gop 60 --preset balanced)

echo "== encode (CRF36, background_qp=14 via JSON) =="
"$BIN" --in "$INPUT" --roi "$SAMPLES" --out "$OUT/adaptive.mp4" "${COMMON[@]}"
"$BIN" --in "$INPUT" --roi "$BASE_JSON" --out "$OUT/baseline.mp4" "${COMMON[@]}"

bytes() { wc -c <"$1" | tr -d ' '; }
BR() {
  local f=$1
  python3 -c "import subprocess; b=int($(bytes "$f")); d=float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration','-of','default=nw=1:nk=1','$f']).decode()); print(f'{8*b/d:.0f}')"
}

echo "sizes: baseline=$(bytes "$OUT/baseline.mp4") (~$(BR "$OUT/baseline.mp4") bps)"
echo "       adaptive=$(bytes "$OUT/adaptive.mp4") (~$(BR "$OUT/adaptive.mp4") bps)"

parse_metric() {
  local file=$1 key=$2
  "$METRICS" "$INPUT" "$file" "$TRUTH" v6 2>/dev/null \
    | grep -oE "${key}=[-0-9.]+" | head -1 | cut -d= -f2
}

echo ""
echo "== roi_metrics vs source (§40 pair: ROI vs background) =="
echo "--- baseline (no ROI regions) ---"
"$METRICS" "$INPUT" "$OUT/baseline.mp4" "$TRUTH" baseline
echo "--- adaptive (v3 ladder + ROI) ---"
"$METRICS" "$INPUT" "$OUT/adaptive.mp4" "$TRUTH" adaptive \
  --baseline "$OUT/baseline.mp4"

CSV="$OUT/metrics.csv"
echo "mode,bytes,bps,roi_psnr,bg_psnr,separation,roi_ssim,bg_ssim" >"$CSV"
for mode in baseline adaptive; do
  f="$OUT/${mode}.mp4"
  roi=$(parse_metric "$f" roi_psnr)
  bg=$(parse_metric "$f" bg_psnr)
  sep=$(parse_metric "$f" separation)
  rssim=$(parse_metric "$f" roi_ssim)
  bssim=$(parse_metric "$f" bg_ssim)
  echo "${mode},$(bytes "$f"),$(BR "$f"),${roi},${bg},${sep},${rssim},${bssim}" >>"$CSV"
done

echo ""
echo "== summary (see $CSV) =="
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"

python3 - "$CSV" <<'PY'
import csv, sys
rows = {r["mode"]: r for r in csv.DictReader(open(sys.argv[1]))}
b, a = rows["baseline"], rows["adaptive"]
roi_b, roi_a = float(b["roi_psnr"]), float(a["roi_psnr"])
bg_b, bg_a = float(b["bg_psnr"]), float(a["bg_psnr"])
bytes_b, bytes_a = int(b["bytes"]), int(a["bytes"])
print()
print("§16-style check (same CRF, not yet matched VBR):")
print(f"  ROI PSNR:  {roi_b:.3f} -> {roi_a:.3f} dB  ({'PASS' if roi_a > roi_b else 'FAIL'})")
print(f"  BG PSNR:   {bg_b:.3f} -> {bg_a:.3f} dB  ({'PASS' if bg_a < bg_b else 'note: bg did not pay'})")
diff = abs(bytes_a - bytes_b) / bytes_b * 100 if bytes_b else 0
print(f"  size diff: {diff:.2f}%")
PY
