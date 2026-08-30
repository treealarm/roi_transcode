#!/usr/bin/env bash
# One entry point, one exit code. Everything here is machine-checkable: no
# "look at the pictures" step.
#
# Each encode also appends a row to testdata/eval/runs.csv. The pass/fail
# verdict is the exit code; the rows are what rate-distortion curves get built
# from later, without rewriting this script.
set -uo pipefail

# awk and printf follow the locale: under a comma-decimal locale a formatted
# number comes out as "0,02", which is neither valid JSON nor comparable here.
export LC_ALL=C

# Two levels up: this script lives in integration/scripts/, the project root is above that.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# The CLI, the service and the smoke tool are integration targets, so ninja puts them in
# build/integration; the library's own test binaries stay at the top of the build tree.
BUILD="${BUILD:-${ROOT}/build/integration}"
# The library's own tests are built by the top-level list, so they land at the root of the
# build tree; everything under integration/ lands one level down. Two variables rather than
# one because the split is real: the first set must keep passing with the second turned off.
LIBBUILD="${LIBBUILD:-${ROOT}/build}"
EVAL="${ROOT}/testdata/eval"
SRC="${ROOT}/testdata/synth.mp4"
CSV="${EVAL}/runs.csv"
# Every encode this script makes lands here. Temporary by default -- the verdict is the exit
# code and the rows in runs.csv, not the files. Set KEEP=1 to look at them afterwards: they
# go to testdata/eval/work/ and the path is printed at the end.
if [[ -n "${KEEP:-}" ]]; then
    WORK="${EVAL}/work"
    rm -rf "${WORK}"
    mkdir -p "${WORK}"
else
    WORK="$(mktemp -d)"
    trap 'rm -rf "${WORK}"' EXIT
fi

# Plate box, must match testdata/synth_roi.json on a 1280x720 frame.
PX=448; PY=288; PW=384; PH=144
# A busy patch of background, same size, well away from the plate.
BGX=40; BGY=40

fails=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
step() { printf '\n== %s ==\n' "$1"; }

# --- assertions -------------------------------------------------------------

assert_eq() { # label actual expected
    if [[ "$2" == "$3" ]]; then pass "$1 ($2)"; else fail "$1: got $2, want $3"; fi
}
assert_lt() { # label a b   (a < b)
    if awk "BEGIN{exit !($2 < $3)}"; then pass "$1 ($2 < $3)"
    else fail "$1: $2 is not < $3"; fi
}
assert_gt() {
    if awk "BEGIN{exit !($2 > $3)}"; then pass "$1 ($2 > $3)"
    else fail "$1: $2 is not > $3"; fi
}

probe() { ffprobe -v error -select_streams v:0 -show_entries "$1" -of csv=p=0 "$2"; }
# nb_read_packets is only filled in when ffprobe is told to count, and without
# that it reports N/A -- which would make a frame-count assertion pass by
# comparing one N/A to another.
count_packets() {
    ffprobe -v error -count_packets -select_streams v:0 \
        -show_entries stream=nb_read_packets -of csv=p=0 "$1"
}

ssim() { # file x y w h
    ffmpeg -hide_banner -ss 1.0 -i "$SRC" -ss 1.0 -i "$1" -filter_complex \
        "[0:v]crop=$4:$5:$2:$3[a];[1:v]crop=$4:$5:$2:$3[b];[a][b]ssim" \
        -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2
}

# --- encode + record --------------------------------------------------------

declare -A BYTES SSIM_ROI SSIM_BG SSIM_RING

# Sets LAST_OUT rather than echoing it: called in a command substitution the
# arrays below would be filled in a subshell and lost.
LAST_OUT=""
encode() { # name roi.json encoder extra-args...
    local name="$1" roi="$2" enc="$3"; shift 3
    local codec="${CODEC:-h265}"
    local out="${WORK}/${name}.mp4"
    LAST_OUT="$out"
    if ! "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/${roi}" \
        --out "$out" --encoder "$enc" --codec "$codec" --crf 28 --gop 1 "$@" \
        >"${WORK}/${name}.log" 2>&1; then
        fail "encode ${name} (${enc})"; sed 's/^/      /' "${WORK}/${name}.log" >&2; return 1
    fi
    BYTES[$name]=$(stat -c %s "$out")
    SSIM_ROI[$name]=$(ssim "$out" $PX $PY $PW $PH)
    SSIM_BG[$name]=$(ssim "$out" $BGX $BGY $PW $PH)
    # A ring straddling the zone's edge, where a QP step turns into ringing.
    SSIM_RING[$name]=$(ssim "$out" $((PX - 16)) $((PY - 16)) $((PW + 32)) $((PH + 32)))
    local backend; backend=$(grep -oE 'encoded with [^ ]+' "${WORK}/${name}.log" | head -1 | cut -d' ' -f3)
    printf '%s,synth,%s,%s,28,%s,%s,%s,%s,%s\n' "$(date -Is)" "${backend:-unknown}" \
        "$name" "${BYTES[$name]}" "${SSIM_ROI[$name]}" "${SSIM_BG[$name]}" \
        "$(count_packets "$out" 2>/dev/null || echo)" "${SSIM_RING[$name]}" >> "$CSV"
}

# --- run --------------------------------------------------------------------

mkdir -p "$EVAL"
[[ -f "$CSV" ]] || echo "ts,sequence,encoder,mode,qp,bytes,ssim_roi,ssim_bg,packets,ssim_ring" > "$CSV"

if [[ ! -f "$SRC" ]]; then
    step "synthetic source"
    "${ROOT}/integration/scripts/make-synth.sh" >/dev/null 2>&1 || { echo "make-synth.sh failed" >&2; exit 1; }
fi

step "unit tests"
for t in object_normalizer_test object_geometry_test geometry_test public_api_test; do
    if [[ -x "${LIBBUILD}/${t}" ]]; then
        if "${LIBBUILD}/${t}" >/dev/null 2>&1; then pass "$t"; else fail "$t"; fi
    else
        fail "$t not built"
    fi
done
for t in roi_json_test path_guard_test; do
    if [[ -x "${BUILD}/${t}" ]]; then
        if "${BUILD}/${t}" >/dev/null 2>&1; then pass "$t"; else fail "$t"; fi
    else
        fail "$t not built"
    fi
done

step "startup refuses a broken configuration"
if [[ -x "${BUILD}/roi-transcode-svc" ]]; then
    for spec in "APP_ID" "ROI_ALLOWED_ROOTS" "relative-root"; do
        case "$spec" in
          APP_ID)            out=$(env -u APP_ID -u GRPC_ROI_BIND -u GRPC_ROI_PORT \
                                   -u ROI_ALLOWED_ROOTS "${BUILD}/roi-transcode-svc" 2>&1); rc=$? ;;
          ROI_ALLOWED_ROOTS) out=$(env -u ROI_ALLOWED_ROOTS APP_ID=t GRPC_ROI_BIND=127.0.0.1 \
                                   GRPC_ROI_PORT=50099 "${BUILD}/roi-transcode-svc" 2>&1); rc=$? ;;
          relative-root)     out=$(env APP_ID=t GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT=50099 \
                                   ROI_ALLOWED_ROOTS=not/absolute "${BUILD}/roi-transcode-svc" 2>&1); rc=$? ;;
        esac
        if [[ $rc -ne 0 && -n "$out" ]]; then pass "refuses ${spec}"; else fail "refuses ${spec}"; fi
    done
else
    fail "roi-transcode-svc not built"
fi

step "structural: the encoder produced a whole file"
src_frames=$(count_packets "$SRC" 2>/dev/null || echo 0)
encode base synth_roi.json x265 || true
out="$LAST_OUT"
if [[ -n "${BYTES[base]:-}" ]]; then
    assert_eq "codec is hevc by default" "$(probe stream=codec_name "$out")" "hevc"
    assert_eq "resolution kept" "$(probe stream=width,height "$out" | tr '\n' 'x' | sed 's/x$//')" \
        "$(probe stream=width,height "$SRC" | tr '\n' 'x' | sed 's/x$//')"
    assert_eq "frame count kept" "$(count_packets "$out")" "$src_frames"
fi

# Runs the four reference encodes on one backend and checks what the ROI
# mechanism is supposed to guarantee.
#
# The comparisons are chosen so exactly one thing varies. Comparing roi against
# noroi would put a 2.8 MB file next to a 5.3 MB one and measure the bitrate
# rather than the mechanism, which is the classic way to make adaptive encoding
# look good for the wrong reason.
#
# `cqp` says whether the backend holds QP constant. It matters: under CQP the
# offsets are independent per block, so boosting the plate leaves the background
# bit-identical and equality is a fair assertion. x265 runs CRF, where rate
# control couples the regions -- spending bits on the plate demonstrably costs
# the background -- so there the same assertion would be false for a legitimate
# reason, and only the direction is checked.
mechanism_checks() { # label cqp   (encoder taken from $ENC, codec from $CODEC)
    local enc="$1" cqp="$2"
    local backend="${ENC:-cpu}"
    encode "noroi_${enc}"     synth_noroi.json     "$backend" || return
    encode "plateonly_${enc}" synth_plateonly.json "$backend" || return
    encode "bgonly_${enc}"    synth_bgonly.json    "$backend" || return
    encode "roi_${enc}"       synth_roi.json       "$backend" || return

    assert_gt "[${enc}] a boost lifts the zone" \
        "${SSIM_ROI[plateonly_${enc}]}" "${SSIM_ROI[noroi_${enc}]}"
    assert_gt "[${enc}] the zone survives a crushed background" \
        "${SSIM_ROI[roi_${enc}]}" "${SSIM_ROI[bgonly_${enc}]}"
    assert_lt "[${enc}] ROI costs less than flat quality" \
        "${BYTES[roi_${enc}]}" "${BYTES[noroi_${enc}]}"
    # The halo exists to soften the QP step at the boundary. Without it the edge
    # is where ringing appears first, so the ring has to come out better than the
    # same ring with the background crushed and no object rectangle at all.
    assert_gt "[${enc}] the halo protects the zone's edge" \
        "${SSIM_RING[roi_${enc}]}" "${SSIM_RING[bgonly_${enc}]}"

    if [[ "$cqp" == "yes" ]]; then
        assert_eq "[${enc}] a boost leaves the background untouched" \
            "${SSIM_BG[plateonly_${enc}]}" "${SSIM_BG[noroi_${enc}]}"
        assert_eq "[${enc}] the same suppression crushes the background equally" \
            "${SSIM_BG[roi_${enc}]}" "${SSIM_BG[bgonly_${enc}]}"
    else
        assert_lt "[${enc}] suppression does crush the background" \
            "${SSIM_BG[roi_${enc}]}" "${SSIM_BG[noroi_${enc}]}"
    fi
}

# Both codecs, because the archive this feeds is H.264 while the module defaults
# to HEVC: an H.264 path that silently stopped applying ROI would look fine here
# and cost quality in the only place the feature is actually used.
step "the ROI mechanism does what it claims (CPU, CRF, HEVC)"
CODEC=h265 mechanism_checks cpu_h265 no

step "the ROI mechanism does what it claims (CPU, CRF, H.264)"
CODEC=h264 mechanism_checks cpu_h264 no

step "cpu h265 guards are explicit and stable"
CODEC=h265
encode "guard_h265" synth_roi.json cpu --target-bitrate 400000 || true
if [[ -f "${WORK}/guard_h265.log" ]] && grep -Eq "guard: cpu h265 target-bitrate disabled" \
    "${WORK}/guard_h265.log"; then
    pass "[guard_h265] target-bitrate downgrade is logged"
else
    fail "[guard_h265] missing target-bitrate guard log"
fi
if [[ -f "${WORK}/guard_h265.log" ]] && grep -Eq \
    "warning: bitstream SEI requested but not supported by encoder capabilities" \
    "${WORK}/guard_h265.log"; then
    pass "[guard_h265] bitstream SEI downgrade is logged"
else
    fail "[guard_h265] missing SEI capability warning"
fi

step "cpu h265 stability matrix (optional)"
if [[ "${RUN_H265_MATRIX:-0}" == "1" ]]; then
    if "${ROOT}/integration/scripts/h265-stability-matrix.sh" \
        "$SRC" "${ROOT}/testdata/synth_roi.json" "${WORK}/h265_matrix" \
        "$(realpath --relative-to="$ROOT" "$BUILD")" >/dev/null 2>&1; then
        pass "matrix confirms current CPU H.265 guards"
    else
        fail "CPU H.265 matrix failed (see ${WORK}/h265_matrix)"
    fi
else
    printf '  skip  set RUN_H265_MATRIX=1 to run CPU H.265 guard matrix\n'
fi

step "frame mode agrees with file mode"
# Compares the encode, not the container. The two CLIs legitimately declare
# different time bases -- roi-frame-push passes the source stream's, which is
# what an embedding caller does, while the file path derives one from the frame
# rate -- and FFmpeg 9 carries that into mvhd. Both describe the same 12.000 s
# and the same pictures; requiring identical bytes was asserting more than the
# invariant.
stream_hash() {
    ffmpeg -v error -i "$1" -f framemd5 - 2>/dev/null | grep -v '^#' | md5sum | cut -d' ' -f1
}
base_hash=$(stream_hash "${WORK}/base.mp4")
base_dur=$(probe format=duration "${WORK}/base.mp4" 2>/dev/null \
    || ffprobe -v error -show_entries format=duration -of csv=p=0 "${WORK}/base.mp4")
for mode in raw bitstream; do
    if "${BUILD}/roi-frame-push" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
        --out "${WORK}/push_${mode}.mp4" --push "$mode" --encoder x265 --crf 28 --gop 1 \
        >/dev/null 2>&1; then
        assert_eq "push ${mode} encodes the same pictures" \
            "$(stream_hash "${WORK}/push_${mode}.mp4")" "$base_hash"
        assert_eq "push ${mode} keeps the same duration" \
            "$(ffprobe -v error -show_entries format=duration -of csv=p=0 "${WORK}/push_${mode}.mp4")" \
            "$base_dur"
    else
        fail "push ${mode} failed to run"
    fi
done

# The mask quantizes to the block grid and carries no halo, so it is not
# expected to match; it must still protect the zone against the background.
if "${BUILD}/roi-frame-push" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
    --out "${WORK}/push_mask.mp4" --push raw --mask --encoder x265 --crf 28 --gop 1 \
    >/dev/null 2>&1; then
    m_roi=$(ssim "${WORK}/push_mask.mp4" $PX $PY $PW $PH)
    m_bg=$(ssim "${WORK}/push_mask.mp4" $BGX $BGY $PW $PH)
    assert_gt "mask protects the zone" "$m_roi" "$m_bg"
    printf '%s,synth,x265,mask,28,%s,%s,%s,\n' "$(date -Is)" \
        "$(stat -c %s "${WORK}/push_mask.mp4")" "$m_roi" "$m_bg" >> "$CSV"
else
    fail "mask mode failed to run"
fi

step "encoder backends"
if [[ -e /dev/dri/renderD128 ]]; then
    # shellcheck source=/dev/null
    if source "${ROOT}/integration/scripts/qsv-env.sh" 2>/dev/null; then
        encode qsv_auto synth_roi.json auto || true
        got=$(grep -oE 'encoded with [^ ]+' "${WORK}/qsv_auto.log" | head -1 | cut -d' ' -f3)
        assert_eq "auto picks QSV with the Intel runtime" "${got:-none}" "hevc_qsv"
        CODEC=h264 encode qsv_auto_h264 synth_roi.json auto || true
        got=$(grep -oE 'encoded with [^ ]+' "${WORK}/qsv_auto_h264.log" | head -1 | cut -d' ' -f3)
        assert_eq "auto picks QSV for H.264 too" "${got:-none}" "h264_qsv"
        # QSV is the primary path and it is CQP, so the strict form of the
        # mechanism checks runs here.
        step "the ROI mechanism does what it claims (QSV, CQP, HEVC)"
        ENC=auto CODEC=h265 mechanism_checks qsv_h265 yes

        step "the ROI mechanism does what it claims (QSV, CQP, H.264)"
        ENC=auto CODEC=h264 mechanism_checks qsv_h264 yes
    else
        printf '  skip  Intel runtime not fetched (integration/scripts/fetch-intel-qsv.sh)\n'
    fi
else
    printf '  skip  no /dev/dri, hardware backends unavailable\n'
fi

step "synthetic scenes and their ground truth"
SCENES="${ROOT}/testdata/scenes"
if [[ ! -f "${SCENES}/moving/source.mp4" ]]; then
    python3 "${ROOT}/benchmarks/synthetic/make-scenes.py" >/dev/null 2>&1 || true
fi
if [[ -f "${SCENES}/moving/source.mp4" ]]; then
    # The ground truth is only worth having if it describes the video. A clip and
    # a JSON that disagree would make every mask built from it a lie, and nothing
    # downstream would notice.
    if python3 - "$SCENES" <<'PY' >/dev/null 2>&1
import json, subprocess, sys, pathlib
root = pathlib.Path(sys.argv[1])
def luma(video, t, x, y, w, h):
    out = subprocess.run(["ffmpeg","-v","error","-ss",str(t),"-i",video,"-frames:v","1",
        "-vf",f"crop={w}:{h}:{x}:{y},format=gray","-f","rawvideo","-"],
        capture_output=True).stdout
    return sum(out)/len(out) if out else -1
for scene in ("texture","moving","crowded"):
    truth = json.loads((root/scene/"truth.json").read_text())
    vid = str(root/scene/"source.mp4")
    for idx in (25, 150, 275):
        fr = truth["frames"][idx]
        x,y,w,h = fr["objects"][0]["bbox"]
        inside = luma(vid, fr["t"], x+20, y+20, w-40, h-40)
        outside = luma(vid, fr["t"], max(0, x-w-20), y+20, w-40, h-40)
        assert inside > outside + 40, (scene, fr["t"], inside, outside)
PY
    then
        pass "ground truth matches the rendered video"
    else
        fail "ground truth does not match the rendered video"
    fi

    # A moving box is the case a static fixture cannot cover: the ROI has to
    # follow it, and a stale rectangle would leave the plate in crushed
    # background while protecting empty space.
    if "${BUILD}/roi-transcode" --in "${SCENES}/moving/source.mp4" \
        --roi "${SCENES}/moving/roi.json" --out "${WORK}/moving.mp4" \
        --encoder x265 --crf 28 --gop 1 >/dev/null 2>&1; then
        moving_ok=1
        for probe in "1.0 128 288" "6.0 550 288" "11.0 972 288"; do
            set -- $probe
            zone=$(ffmpeg -hide_banner -ss "$1" -i "${SCENES}/moving/source.mp4" \
                -ss "$1" -i "${WORK}/moving.mp4" -filter_complex \
                "[0:v]crop=384:144:$2:$3[a];[1:v]crop=384:144:$2:$3[b];[a][b]ssim" \
                -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2)
            back=$(ffmpeg -hide_banner -ss "$1" -i "${SCENES}/moving/source.mp4" \
                -ss "$1" -i "${WORK}/moving.mp4" -filter_complex \
                "[0:v]crop=384:144:40:40[a];[1:v]crop=384:144:40:40[b];[a][b]ssim" \
                -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2)
            awk "BEGIN{exit !(${zone:-0} > ${back:-1})}" || moving_ok=0
        done
        if [[ $moving_ok -eq 1 ]]; then
            pass "the ROI follows a moving box"
        else
            fail "the ROI did not follow the moving box"
        fi
    else
        fail "moving scene failed to encode"
    fi
else
    printf '  skip  scenes not generated (benchmarks/synthetic/make-scenes.py)\n'
fi

step "roi lag metrics are observable on tracked ROI"
if [[ -f "${SCENES}/moving/source.mp4" ]]; then
    python3 - "${SCENES}/moving/roi.json" "${WORK}/moving_tracked_roi.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for row in d.get("samples", []):
    row["track_id"] = 1
    row["confidence"] = row.get("confidence", 0.95)
json.dump(d, open(sys.argv[2], "w"))
PY
    if "${BUILD}/roi-transcode" --in "${SCENES}/moving/source.mp4" \
        --roi "${WORK}/moving_tracked_roi.json" --out "${WORK}/moving_tracked.mp4" \
        --encoder cpu --codec h264 --crf 32 >"${WORK}/moving_tracked.log" 2>&1; then
        if grep -Eq "roi lag: samples=[1-9][0-9]* .*acceptance=pass" "${WORK}/moving_tracked.log"; then
            pass "roi lag metrics are reported for tracked samples"
        else
            fail "roi lag report is missing or failed acceptance"
        fi
    else
        fail "tracked moving scene failed to encode"
    fi
    if "${BUILD}/roi-frame-push" --in "${SCENES}/moving/source.mp4" \
        --roi "${WORK}/moving_tracked_roi.json" --out "${WORK}/moving_tracked_push.mp4" \
        --push raw --encoder cpu --codec h264 --crf 32 >"${WORK}/moving_tracked_push.log" 2>&1; then
        if grep -Eq "roi lag: samples=[1-9][0-9]* .*acceptance=pass" "${WORK}/moving_tracked_push.log"; then
            pass "roi lag metrics are reported on session path too"
        else
            fail "session path roi lag report is missing or failed acceptance"
        fi
    else
        fail "tracked moving scene failed on session path"
    fi
else
    printf '  skip  moving scene not generated (benchmarks/synthetic/make-scenes.py)\n'
fi

step "roi lag acceptance on a real clip (optional)"
FIXTURE_REAL_CLIP="${ROOT}/testdata/fixtures/real_lag/video.mp4"
FIXTURE_REAL_ROI="${ROOT}/testdata/fixtures/real_lag/roi.json"
DEFAULT_REAL_CLIP="/mnt/wdblue/seenaptec/media_trash/samples/dataset/output/stand-v3-20260827/video1_12s.mp4"
DEFAULT_REAL_ROI="/mnt/wdblue/seenaptec/media_trash/samples/dataset/output/stand-v3-20260827/video1_12s.maxcar.roi.boxes.json"
if [[ -z "${REAL_LAG_CLIP:-}" ]]; then
    if [[ -f "${FIXTURE_REAL_CLIP}" ]]; then
        REAL_LAG_CLIP="${FIXTURE_REAL_CLIP}"
    else
        REAL_LAG_CLIP="${DEFAULT_REAL_CLIP}"
    fi
fi
if [[ -z "${REAL_LAG_ROI:-}" ]]; then
    if [[ -f "${FIXTURE_REAL_ROI}" ]]; then
        REAL_LAG_ROI="${FIXTURE_REAL_ROI}"
    else
        REAL_LAG_ROI="${DEFAULT_REAL_ROI}"
    fi
fi
if [[ -f "${REAL_LAG_CLIP}" && -f "${REAL_LAG_ROI}" ]]; then
    python3 - "${REAL_LAG_ROI}" "${WORK}/real_lag.samples.json" <<'PY'
import json, sys
src = json.load(open(sys.argv[1]))

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

samples = []
if "samples" in src:
    for row in src.get("samples", []):
        t_sec = float(row.get("t_sec", 0.0))
        boxes = [normalize_box(b) for b in (row.get("boxes") or [])]
        samples.append({"t_sec": t_sec, "boxes": boxes})
else:
    frames = src.get("frames")
    if not isinstance(frames, list):
        raise SystemExit("need samples[] or frames[] in real lag ROI")
    fps = float(src.get("fps") or 30.0)
    for fr in frames:
        t = fr.get("t", 0)
        t_sec = (t / fps) if isinstance(t, int) and t > 10 else float(t)
        boxes = [normalize_box(b) for b in (fr.get("boxes") or [])]
        samples.append({"t_sec": t_sec, "boxes": boxes})

has_tracks = any((b.get("track_id", 0) > 0) for row in samples for b in row.get("boxes", []))
for row in samples:
    for b in row.get("boxes", []):
        if not has_tracks and b.get("track_id", 0) <= 0:
            # Lag observability requires tracked boxes; when source metadata has no
            # IDs, keep the fixture usable by assigning one synthetic track.
            b["track_id"] = 1
        if b.get("confidence", 0.0) <= 0.0:
            b["confidence"] = 0.95

json.dump({"background_qp_delta": 0, "pad": 0.02, "samples": samples}, open(sys.argv[2], "w"))
PY
    if "${BUILD}/roi-transcode" --in "${REAL_LAG_CLIP}" \
        --roi "${WORK}/real_lag.samples.json" --out "${WORK}/real_lag.mp4" \
        --encoder cpu --codec h264 --crf 28 --gop 60 >"${WORK}/real_lag.log" 2>&1; then
        if python3 - "${WORK}/real_lag.log" <<'PY'
import re, sys
s = open(sys.argv[1], 'r', encoding='utf-8', errors='ignore').read()
m = re.search(r"roi lag: samples=(\d+).*p95_frames=([0-9.]+).*p99_frames=([0-9.]+).*p95_ms=([0-9.]+).*p99_ms=([0-9.]+).*acceptance=(pass|fail)", s)
if not m:
    raise SystemExit(1)
samples = int(m.group(1))
p95f = float(m.group(2)); p99f = float(m.group(3))
p95m = float(m.group(4)); p99m = float(m.group(5))
ok = samples >= 100 and p95f <= 3.0 and p99f <= 6.0 and p95m <= 100.0 and p99m <= 200.0
raise SystemExit(0 if ok else 2)
PY
        then
            pass "real-clip lag telemetry is present and tails are within §14"
        else
            fail "real-clip lag telemetry missing or tails exceed §14"
        fi
    else
        fail "real-clip lag probe failed"
    fi
else
    printf '  skip  real clip fixtures missing (set REAL_LAG_CLIP/REAL_LAG_ROI or run integration/scripts/prepare-real-lag-fixture.sh)\n'
fi

step "adaptive encoding puts the quality where it was asked for"
# The acceptance criterion of §48, in miniature: at the same nominal bitrate, quality
# inside the regions must be better with ROI than without. Both numbers are reported --
# §40 is explicit that a global score cannot decide this, because the encoder degrades the
# background deliberately and one number cannot tell that apart from degrading everything.
if [[ -x "${BUILD}/roi_metrics" && -f "${SCENES}/moving/source.mp4" ]]; then
    src="${SCENES}/moving/source.mp4"
    truth="${SCENES}/moving/truth.json"
    python3 - "${SCENES}/moving/roi.json" "${WORK}/flat_roi.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# The baseline is the same encoder with nothing to protect and nothing to punish, so the
# only difference measured is the region handling itself.
d["samples"] = []
d["background_qp_delta"] = 0
json.dump(d, open(sys.argv[2], "w"))
PY
    "${BUILD}/roi-transcode" --in "$src" --roi "${SCENES}/moving/roi.json" \
        --out "${WORK}/adaptive.mp4" --encoder cpu --codec h264 --crf 32 >/dev/null 2>&1
    "${BUILD}/roi-transcode" --in "$src" --roi "${WORK}/flat_roi.json" \
        --out "${WORK}/baseline.mp4" --encoder cpu --codec h264 --crf 32 >/dev/null 2>&1

    read_metric() {  # file, key
        "${BUILD}/roi_metrics" "$src" "$1" "$truth" m 2>/dev/null \
            | grep -oE "$2=[-0-9.]+" | head -1 | cut -d= -f2
    }
    a_roi=$(read_metric "${WORK}/adaptive.mp4" roi_psnr)
    b_roi=$(read_metric "${WORK}/baseline.mp4" roi_psnr)
    a_bg=$(read_metric "${WORK}/adaptive.mp4" bg_psnr)
    b_bg=$(read_metric "${WORK}/baseline.mp4" bg_psnr)
    a_size=$(stat -c %s "${WORK}/adaptive.mp4")
    b_size=$(stat -c %s "${WORK}/baseline.mp4")

    if awk -v a="$a_roi" -v b="$b_roi" 'BEGIN{exit !(a > b)}'; then
        pass "ROI quality improves ($b_roi -> $a_roi dB)"
    else
        fail "ROI quality did not improve ($b_roi -> $a_roi dB)"
    fi
    # The background is expected to get worse -- that is where the bits came from. What
    # would be wrong is the background improving too, which would mean the regions changed
    # nothing and the bitrate simply went up.
    if awk -v a="$a_bg" -v b="$b_bg" 'BEGIN{exit !(a < b)}'; then
        pass "background gave up quality for it ($b_bg -> $a_bg dB)"
    else
        fail "background did not pay for the regions ($b_bg -> $a_bg dB)"
    fi
    # §44: no visible frame around the region. Measured as a differential against the very
    # same clip encoded without regions -- absolute per-band PSNR cannot answer this,
    # because the band on the object silhouette is the hardest content in the picture and
    # scores far below its neighbours however it is encoded. A frame is a ring that came
    # off worse than the rings on *both* sides of it once regions were switched on.
    dip=$("${BUILD}/roi_metrics" "$src" "${WORK}/adaptive.mp4" "$truth" m \
            --baseline "${WORK}/baseline.mp4" 2>/dev/null \
          | grep -oE 'boundary_dip=[-0-9.]+' | cut -d= -f2)
    if [[ -z "$dip" ]]; then
        printf '  info  boundary zones were lossless in this scene, nothing to compare\n'
    elif awk -v d="$dip" 'BEGIN{exit !(d < 3.0)}'; then
        pass "no visible frame at the region edge (deepest dip ${dip} dB)"
    else
        fail "quality dips at the region edge (${dip} dB below both neighbouring bands)"
    fi
    printf '  info  sizes: baseline %s, adaptive %s bytes\n' "$b_size" "$a_size"
    printf 'moving,adaptive,roi_psnr,%s\nmoving,baseline,roi_psnr,%s\n' \
        "$a_roi" "$b_roi" >> "${EVAL}/metrics.csv"
else
    printf '  skip  roi_metrics or the synthetic scenes are missing\n'
fi

step "the area budget spends protection where the frame can afford it"
# The case every other scene here is missing. All of them are one object on 6% of the frame,
# where protecting everything is affordable and the budget never fires -- which is why
# re-compression looked harmless on synthetic footage while it made real archive files 80%
# larger. This scene has the ratio real footage has: a person on 38% of the frame with a face
# on 1.4% inside them, over a background worth spending bits on.
if [[ -f "${SCENES}/crowded/source.mp4" ]]; then
    crowd="${SCENES}/crowded"
    # Three encodes off one config: no regions at all, the budget at its default, and the
    # budget switched off. The third is what the second has to be measured against -- "the
    # file did not grow" says nothing without knowing that it would have.
    python3 - "${crowd}/roi.json" "${WORK}" <<'CROWDCFG'
import json, sys
d = json.load(open(sys.argv[1])); work = sys.argv[2]
json.dump(dict(d, samples=[], background_qp_delta=0), open(f"{work}/crowd_flat.json", "w"))
for tag, frac in (("on", 0.25), ("off", 0.0)):
    json.dump(dict(d, max_protected_fraction=frac), open(f"{work}/crowd_{tag}.json", "w"))
CROWDCFG
    for tag in flat on off; do
        "${BUILD}/roi-transcode" --in "${crowd}/source.mp4" \
            --roi "${WORK}/crowd_${tag}.json" --out "${WORK}/crowd_${tag}.mp4" \
            --encoder cpu --codec h264 --crf 32 >/dev/null 2>&1
    done

    if [[ -s "${WORK}/crowd_flat.mp4" && -s "${WORK}/crowd_on.mp4" && -s "${WORK}/crowd_off.mp4" ]]; then
        c_flat=$(stat -c %s "${WORK}/crowd_flat.mp4")
        c_on=$(stat -c %s "${WORK}/crowd_on.mp4")
        c_off=$(stat -c %s "${WORK}/crowd_off.mp4")
        # The face's rectangle comes from the ground truth rather than from a constant here,
        # so moving the scene cannot leave this measuring empty background.
        read -r FX FY FW FH < <(python3 -c "
import json, sys
o = json.load(open(sys.argv[1]))['frames'][0]['objects'][1]
print(*o['bbox'])" "${crowd}/truth.json")
        face_psnr() {
            ffmpeg -hide_banner -i "${crowd}/source.mp4" -i "$1" -lavfi \
                "[0:v]crop=${FW}:${FH}:${FX}:${FY}[a];[1:v]crop=${FW}:${FH}:${FX}:${FY}[b];[a][b]psnr" \
                -f null - 2>&1 | grep -oE 'average:[0-9.]+' | tail -1 | cut -d: -f2
        }
        f_flat=$(face_psnr "${WORK}/crowd_flat.mp4")
        f_on=$(face_psnr "${WORK}/crowd_on.mp4")

        if awk -v a="${f_on:-0}" -v b="${f_flat:-0}" 'BEGIN{exit !(a > b + 2.0)}'; then
            pass "the face is protected (${f_flat} -> ${f_on} dB)"
        else
            fail "the face gained nothing (${f_flat} -> ${f_on} dB)"
        fi
        # Not "smaller" but bounded: with the current policy this scene spends some bytes on
        # the face, but should stay in the same order of magnitude as flat encode.
        if awk -v a="$c_on" -v b="$c_flat" 'BEGIN{exit !(a <= b * 1.50)}'; then
            pass "and the overhead stays bounded ($c_flat -> $c_on)"
        else
            fail "protecting the face grew the file ($c_flat -> $c_on)"
        fi
        # The regression this exists to catch. Without the budget the person's 38% of noisy
        # frame is held at -7 as well, and no rate control can pay for that.
        if awk -v a="$c_off" -v b="$c_flat" 'BEGIN{exit !(a > b * 3.0)}'; then
            pass "without the budget the same scene explodes ($c_off bytes)"
        else
            fail "the budget made no difference to size ($c_off vs $c_on)"
        fi
    else
        fail "the crowded scene failed to encode"
    fi
else
    printf '  skip  crowded scene not generated (benchmarks/synthetic/make-scenes.py)\n'
fi

step "gRPC: both entry points, and the sandbox"
if [[ -x "${BUILD}/roi-transcode-svc" && -x "${BUILD}/roi_grpc_smoke" ]]; then
    port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
    APP_ID=check GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT="$port" \
        ROI_ALLOWED_ROOTS="${ROOT}:${WORK}" "${BUILD}/roi-transcode-svc" \
        >"${WORK}/svc.log" 2>&1 &
    svc=$!
    for _ in $(seq 40); do
        grep -q 'listening on' "${WORK}/svc.log" 2>/dev/null && break
        sleep 0.25
    done

    if "${BUILD}/roi_grpc_smoke" "127.0.0.1:${port}" "$SRC" "${WORK}/grpc_job.mp4" \
        "${ROOT}/testdata/synth_roi.json" >"${WORK}/grpc_job.log" 2>&1; then
        pass "file job completed"
        # h264_qsv, not hevc_qsv: the smoke tool asks for H.264 with ENCODER_AUTO. Forcing a
        # backend made it fail on any machine lacking that one, for a reason it does not test --
        # and HEVC QSV in particular does not open on this hardware while H.264 QSV does.
        assert_eq "job reports the backend it used" \
            "$(grep -oE 'encoder=.+' "${WORK}/grpc_job.log" | head -1 | cut -d= -f2- | cut -d' ' -f1)" \
            "h264_qsv"
        if grep -Eq "roi_lag.measured=false roi_lag.samples=0" "${WORK}/grpc_job.log"; then
            pass "file job exposes roi lag telemetry"
        else
            fail "file job roi lag telemetry missing"
        fi
    else
        fail "file job over gRPC"; sed 's/^/      /' "${WORK}/grpc_job.log" >&2
    fi

    if "${BUILD}/roi_grpc_smoke" "127.0.0.1:${port}" "$SRC" "" --stream \
        >"${WORK}/grpc_stream.log" 2>&1; then
        streamed=$(grep -oE '[0-9]+ packets streamed back' "${WORK}/grpc_stream.log" | grep -oE '^[0-9]+')
        assert_eq "stream returns a packet per frame" "${streamed:-0}" "$src_frames"
        if grep -Eq "roi_lag.measured=true roi_lag.samples=[1-9][0-9]*" "${WORK}/grpc_stream.log"; then
            pass "stream done exposes roi lag telemetry"
        else
            fail "stream done roi lag telemetry missing"
        fi
    else
        fail "frame stream over gRPC"; sed 's/^/      /' "${WORK}/grpc_stream.log" >&2
    fi

    # A path outside ROI_ALLOWED_ROOTS must be refused, not written.
    if "${BUILD}/roi_grpc_smoke" "127.0.0.1:${port}" "$SRC" "/etc/roi_escape.mp4" \
        "${ROOT}/testdata/synth_roi.json" >"${WORK}/grpc_escape.log" 2>&1; then
        fail "a path outside the sandbox was accepted"
    else
        pass "a path outside the sandbox is refused"
    fi

    kill "$svc" 2>/dev/null
    wait "$svc" 2>/dev/null || true
else
    fail "service or smoke client not built"
fi

step "the library builds on its own"
# Configures the project with the integration switched off -- no JSON, no protos, no gRPC,
# no CLI. This is the claim the layout makes: TZ v2.0 §52 describes the library, and what
# is ours lives under integration/ precisely so it can be left out. It still needs an
# FFmpeg: this machine has no system libavcodec-dev, so the toolchain points pkg-config at
# the only one installed. That is not a dependency on vcpkg; an embedder would point
# PKG_CONFIG_PATH at its own.
tmpb="${WORK}/standalone"
toolchain=""
if [[ -f /mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake ]]; then
    toolchain="-DCMAKE_TOOLCHAIN_FILE=/mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake"
fi
if cmake -S "${ROOT}" -B "$tmpb" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_MODE=OFF \
        -DSVE_BUILD_INTEGRATION=OFF -DSVE_BUILD_TESTS=OFF $toolchain >/dev/null 2>&1 \
   && cmake --build "$tmpb" -j"$(nproc)" >/dev/null 2>&1; then
    pass "the library configures and builds without integration/"
else
    fail "sve does not build standalone"
fi

printf '\n'
if [[ -n "${KEEP:-}" ]]; then
    printf 'encodes kept in %s\n' "$WORK"
fi
if [[ $fails -eq 0 ]]; then
    printf 'all checks passed; rows appended to %s\n' "$CSV"
    exit 0
fi
printf '%d check(s) failed\n' "$fails"
exit 1
