#!/usr/bin/env bash
# Build a portable "real lag" fixture for check.sh from local media.
#
# Usage:
#   prepare-real-lag-fixture.sh <source.mp4> <roi.json> [out_dir] [start_sec] [duration_sec]
#
# The output defaults to:
#   testdata/fixtures/real_lag/video.mp4
#   testdata/fixtures/real_lag/roi.json
#
# ROI input may be either:
#   - samples[] (library shape), or
#   - frames[]  (analytics shape).
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <source.mp4> <roi.json> [out_dir] [start_sec] [duration_sec]" >&2
    exit 2
fi

SRC=$(readlink -f "$1")
ROI_IN=$(readlink -f "$2")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT_DIR="${3:-${ROOT}/testdata/fixtures/real_lag}"
START_SEC="${4:-0}"
DUR_SEC="${5:-12}"

if [[ ! -f "$SRC" ]]; then
    echo "missing source clip: $SRC" >&2
    exit 1
fi
if [[ ! -f "$ROI_IN" ]]; then
    echo "missing ROI JSON: $ROI_IN" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
OUT_CLIP="${OUT_DIR}/video.mp4"
OUT_ROI="${OUT_DIR}/roi.json"
TMP_ROI="${OUT_DIR}/roi.full.samples.json"

# Re-encode a bounded segment so every machine gets one stable local fixture.
ffmpeg -y -hide_banner -loglevel error \
  -ss "$START_SEC" -i "$SRC" -t "$DUR_SEC" \
  -an -c:v libx264 -crf 18 -preset veryfast \
  "$OUT_CLIP"

python3 - "$ROI_IN" "$TMP_ROI" "$OUT_ROI" "$START_SEC" "$DUR_SEC" <<'PY'
import json, sys
roi_in, tmp_samples, out_path, start_sec, dur_sec = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])

src = json.load(open(roi_in))
def normalize_box(b):
    return {
        "kind": b.get("kind", ""),
        "x": b.get("x", 0.0),
        "y": b.get("y", 0.0),
        "width": b.get("w", b.get("width", 0.0)),
        "height": b.get("h", b.get("height", 0.0)),
        "track_id": int(b.get("track_id") or 0),
        "confidence": float(b.get("confidence") or 0.0),
    }

samples_in = []
if "samples" in src:
    for row in src.get("samples", []):
        t_sec = float(row.get("t_sec", 0.0))
        boxes = [normalize_box(b) for b in (row.get("boxes") or [])]
        samples_in.append({"t_sec": t_sec, "boxes": boxes})
else:
    frames = src.get("frames")
    if not isinstance(frames, list):
        raise SystemExit("need samples[] or frames[] in ROI JSON")
    fps = float(src.get("fps") or 30.0)
    for fr in frames:
        t = fr.get("t", 0)
        t_sec = (t / fps) if isinstance(t, int) and t > 10 else float(t)
        boxes = [normalize_box(b) for b in (fr.get("boxes") or [])]
        samples_in.append({"t_sec": t_sec, "boxes": boxes})

has_tracks = any((b.get("track_id", 0) > 0) for row in samples_in for b in row.get("boxes", []))
for row in samples_in:
    for b in row.get("boxes", []):
        if not has_tracks and b.get("track_id", 0) <= 0:
            # Keep real-lag fixtures measurable even when source annotations are
            # detection-only and do not include tracking IDs.
            b["track_id"] = 1
        if b.get("confidence", 0.0) <= 0.0:
            b["confidence"] = 0.95

json.dump({"background_qp_delta": 0, "pad": 0.02, "samples": samples_in},
          open(tmp_samples, "w"))

window_end = start_sec + dur_sec
trimmed = []
for row in samples_in:
    t = float(row.get("t_sec", 0.0))
    if t < start_sec or t > window_end:
        continue
    trimmed.append({
        "t_sec": t - start_sec,
        "boxes": row.get("boxes", []),
    })

if not trimmed:
    raise SystemExit("no ROI samples in selected time window")

json.dump({"background_qp_delta": 0, "pad": 0.02, "samples": trimmed},
          open(out_path, "w"))
print(f"wrote {len(trimmed)} samples")
PY

rm -f "$TMP_ROI"
echo "fixture clip: $OUT_CLIP"
echo "fixture roi : $OUT_ROI"
