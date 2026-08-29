#!/usr/bin/env bash
# Matched-bitrate smoke for TZ v3 §16: adaptive vs baseline within 2%.
#
# Uses the offline roi-transcode CLI (samples JSON). Detection is not re-run.
#
# Usage: matched-bitrate.sh <input.mp4> <roi.json> <out_dir> [target_bps] [target_tol_frac]
#
# roi.json: library shape { "samples": [...] } or analytics-worker { "frames": [...] }.
# If target_bps is omitted / 0, the script first measures unconstrained CRF28 bitrates
# and picks target = 70% of min(baseline, adaptive) so the outer loop actually works.
# target_tol_frac: allowed relative miss to requested target (default 0.20).
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 <input.mp4> <roi.json> <out_dir> [target_bps] [target_tol_frac]" >&2
  exit 2
fi

INPUT=$(readlink -f "$1")
BOXES_IN=$(readlink -f "$2")
OUT=$3
TARGET=${4:-0}
TARGET_TOL=${5:-0.20}

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="${ROI_TRANSCODE_BIN:-${ROOT}/build/integration/roi-transcode}"
mkdir -p "$OUT"

if [[ ! -x "$BIN" ]]; then
  echo "no roi-transcode at $BIN (build target roi-transcode, or set ROI_TRANSCODE_BIN)" >&2
  exit 1
fi

SAMPLES="$OUT/adaptive.samples.json"
python3 - "$BOXES_IN" "$SAMPLES" <<'PY'
import json, sys
src_path, dst_path = sys.argv[1], sys.argv[2]
doc = json.load(open(src_path))
if "samples" in doc:
    json.dump(doc, open(dst_path, "w"))
    raise SystemExit(0)
frames = doc.get("frames")
if not isinstance(frames, list):
    raise SystemExit(f"{src_path}: need samples[] or frames[]")
fps = float(doc.get("fps") or 30)
samples = []
for fr in frames:
    t = fr.get("t", 0)
    if isinstance(t, int) and t > 10:
        t_sec = t / fps
    else:
        t_sec = float(t)
    boxes = []
    for b in fr.get("boxes") or []:
        boxes.append({
            "kind": b.get("kind", ""),
            "x": b["x"], "y": b["y"],
            "width": b.get("w", b.get("width", 0)),
            "height": b.get("h", b.get("height", 0)),
            "track_id": int(b.get("track_id") or 0),
            "confidence": float(b.get("confidence") or 0),
        })
    samples.append({"t_sec": t_sec, "boxes": boxes})
out = {"background_qp_delta": 0, "pad": 0.02, "samples": samples}
json.dump(out, open(dst_path, "w"))
print(f"converted {len(samples)} frames → samples", file=sys.stderr)
PY

BASE_JSON="$OUT/baseline_empty.json"
printf '%s\n' '{"background_qp_delta":0,"pad":0.02,"samples":[]}' >"$BASE_JSON"

COMMON=(--encoder cpu --codec h264 --crf 28 --gop 60 --preset balanced)

bps_of() {
  local f=$1
  local bytes dur
  bytes=$(wc -c <"$f" | tr -d ' ')
  dur=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$f")
  python3 -c "print(int(8.0 * $bytes / float('$dur')))"
}

if [[ "$TARGET" == "0" ]]; then
  echo "== probe unconstrained CRF28 (to pick a stressing target) =="
  "$BIN" --in "$INPUT" --roi "$BASE_JSON" --out "$OUT/probe_baseline.mp4" "${COMMON[@]}"
  "$BIN" --in "$INPUT" --roi "$SAMPLES" --out "$OUT/probe_adaptive.mp4" "${COMMON[@]}"
  BR_B=$(bps_of "$OUT/probe_baseline.mp4")
  BR_A=$(bps_of "$OUT/probe_adaptive.mp4")
  echo "unconstrained baseline_bps≈$BR_B adaptive_bps≈$BR_A"
  TARGET=$(python3 -c "print(int(0.70 * min($BR_B, $BR_A)))")
  echo "auto target_bps=$TARGET (70% of the quieter unconstrained encode)"
fi

echo "== matched encode at target_bps=$TARGET =="
"$BIN" --in "$INPUT" --roi "$BASE_JSON" --out "$OUT/baseline.mp4" \
  "${COMMON[@]}" --target-bitrate "$TARGET"
"$BIN" --in "$INPUT" --roi "$SAMPLES" --out "$OUT/adaptive.mp4" \
  "${COMMON[@]}" --target-bitrate "$TARGET"

B=$(wc -c <"$OUT/baseline.mp4" | tr -d ' ')
A=$(wc -c <"$OUT/adaptive.mp4" | tr -d ' ')
DUR=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$OUT/baseline.mp4")

python3 - "$B" "$A" "$DUR" "$TARGET" "$TARGET_TOL" <<'PY'
import sys
b, a, dur, target, tol = (
    float(sys.argv[1]),
    float(sys.argv[2]),
    float(sys.argv[3]),
    float(sys.argv[4]),
    float(sys.argv[5]),
)
print(f"baseline_bytes={int(b)} adaptive_bytes={int(a)}")
br_b = 8.0 * b / dur
br_a = 8.0 * a / dur
print(f"duration_s={dur:.3f}")
print(f"baseline_bps≈{br_b:.0f} adaptive_bps≈{br_a:.0f}")
print(f"requested_target_bps={int(target)}")
diff = abs(br_a - br_b) / br_b if br_b > 0 else 0.0
print(f"relative_diff={diff*100:.2f}% (v3 §16 limit 2.00%)")
# Second gate: both streams should stay close to requested target.
target_fail = False
print(f"target_tolerance={tol*100:.2f}%")
for name, br in (("baseline", br_b), ("adaptive", br_a)):
    d = abs(br - target) / target if target > 0 else 0.0
    print(f"{name}_vs_target={d*100:.2f}%")
    if target > 0 and d > tol:
        target_fail = True
if diff > 0.02:
    sys.exit(1)
if target_fail:
    sys.exit(1)
sys.exit(0)
PY
