#!/usr/bin/env bash
# Probe CPU H.265 guard behavior and emit a compact matrix report.
#
# Usage:
#   h265-stability-matrix.sh <clip.mp4> <roi.json> <out_dir> [build_dir]
#
# The script runs two cases:
#   1) plain CRF encode (CPU H.265),
#   2) target-bitrate request (must hit the guard downgrade).
#
# It writes:
#   - <out_dir>/h265_stability_matrix.csv
#   - per-case logs and outputs
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 <clip.mp4> <roi.json> <out_dir> [build_dir]" >&2
    exit 2
fi

CLIP="$1"
ROI="$2"
OUT="$3"
BUILD_DIR="${4:-build/integration}"

if [[ ! -f "$CLIP" ]]; then
    echo "missing clip: $CLIP" >&2
    exit 1
fi
if [[ ! -f "$ROI" ]]; then
    echo "missing roi json: $ROI" >&2
    exit 1
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="${ROOT}/${BUILD_DIR}/roi-transcode"
if [[ ! -x "$BIN" ]]; then
    echo "missing roi-transcode binary: $BIN" >&2
    exit 1
fi

mkdir -p "$OUT"
CSV="${OUT}/h265_stability_matrix.csv"
printf "case,exit_code,bytes,target_guard,sei_warning,encoder_line,roi_lag_line\n" > "$CSV"

run_case() { # name [extra args...]
    local name="$1"; shift || true
    local out_mp4="${OUT}/${name}.mp4"
    local log="${OUT}/${name}.log"
    local exit_code=0
    set +e
    "$BIN" --in "$CLIP" --roi "$ROI" --out "$out_mp4" \
        --encoder cpu --codec h265 --crf 28 --gop 60 "$@" >"$log" 2>&1
    exit_code=$?
    set -e

    local bytes=0
    if [[ -f "$out_mp4" ]]; then
        bytes=$(stat -c %s "$out_mp4")
    fi
    local target_guard="false"
    local sei_warning="false"
    if grep -Eq "guard: cpu h265 target-bitrate disabled" "$log"; then
        target_guard="true"
    fi
    if grep -Eq "warning: bitstream SEI requested but not supported by encoder capabilities" "$log"; then
        sei_warning="true"
    fi

    local encoder_line
    encoder_line=$(python3 - "$log" <<'PY'
import sys
for line in open(sys.argv[1], "r", encoding="utf-8", errors="ignore"):
    if line.startswith("encoded with "):
        print(line.strip())
        raise SystemExit(0)
print("")
PY
)
    local lag_line
    lag_line=$(python3 - "$log" <<'PY'
import sys
for line in open(sys.argv[1], "r", encoding="utf-8", errors="ignore"):
    if line.startswith("roi lag: "):
        print(line.strip())
        raise SystemExit(0)
print("")
PY
)
    encoder_line=${encoder_line//\"/\'}
    lag_line=${lag_line//\"/\'}
    printf "%s,%d,%s,%s,%s,\"%s\",\"%s\"\n" \
        "$name" "$exit_code" "$bytes" "$target_guard" "$sei_warning" \
        "$encoder_line" "$lag_line" >> "$CSV"

    echo "$name: exit=$exit_code bytes=$bytes target_guard=$target_guard sei_warning=$sei_warning"
}

run_case "cpu_h265_crf"
run_case "cpu_h265_target" --target-bitrate 400000

if python3 - "$CSV" <<'PY'
import csv, sys
rows = {r["case"]: r for r in csv.DictReader(open(sys.argv[1]))}
required = {"cpu_h265_crf", "cpu_h265_target"}
if set(rows) != required:
    raise SystemExit(2)
ok = True
ok &= rows["cpu_h265_crf"]["exit_code"] == "0"
ok &= rows["cpu_h265_crf"]["sei_warning"] == "true"
ok &= rows["cpu_h265_target"]["exit_code"] == "0"
ok &= rows["cpu_h265_target"]["target_guard"] == "true"
ok &= rows["cpu_h265_target"]["sei_warning"] == "true"
raise SystemExit(0 if ok else 1)
PY
then
    echo "h265 stability matrix: PASS"
    exit 0
fi

echo "h265 stability matrix: FAIL (see $CSV and per-case logs)" >&2
exit 1
