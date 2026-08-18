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

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
EVAL="${ROOT}/testdata/eval"
SRC="${ROOT}/testdata/synth.mp4"
CSV="${EVAL}/runs.csv"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

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
    "${ROOT}/scripts/make-synth.sh" >/dev/null 2>&1 || { echo "make-synth.sh failed" >&2; exit 1; }
fi

step "unit tests"
for t in roi_map_builder_test geometry_test roi_json_test path_guard_test; do
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
    if source "${ROOT}/scripts/qsv-env.sh" 2>/dev/null; then
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
        printf '  skip  Intel runtime not fetched (scripts/fetch-intel-qsv.sh)\n'
    fi
else
    printf '  skip  no /dev/dri, hardware backends unavailable\n'
fi

step "synthetic scenes and their ground truth"
SCENES="${ROOT}/testdata/scenes"
if [[ ! -f "${SCENES}/moving/source.mp4" ]]; then
    python3 "${ROOT}/scripts/make-scenes.py" >/dev/null 2>&1 || true
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
for scene in ("texture","moving"):
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
    printf '  skip  scenes not generated (scripts/make-scenes.py)\n'
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
        assert_eq "job reports the backend it used" \
            "$(grep -oE 'encoder=.+' "${WORK}/grpc_job.log" | head -1 | cut -d= -f2- | cut -d' ' -f1)" \
            "hevc_qsv"
    else
        fail "file job over gRPC"; sed 's/^/      /' "${WORK}/grpc_job.log" >&2
    fi

    if "${BUILD}/roi_grpc_smoke" "127.0.0.1:${port}" "$SRC" "" --stream \
        >"${WORK}/grpc_stream.log" 2>&1; then
        streamed=$(grep -oE '[0-9]+ packets streamed back' "${WORK}/grpc_stream.log" | grep -oE '^[0-9]+')
        assert_eq "stream returns a packet per frame" "${streamed:-0}" "$src_frames"
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

step "the core builds on its own"
# core/ is its own CMake project, so this configures *only* it -- no JSON, no
# protos, no gRPC, no CLI, nothing from the root list. It still needs an FFmpeg:
# this machine has no system libavcodec-dev, so the toolchain points pkg-config
# at the only one installed. That is not a dependency on vcpkg; an embedding
# project would point PKG_CONFIG_PATH at its own.
tmpb="${WORK}/standalone"
toolchain=""
if [[ -f /mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake ]]; then
    toolchain="-DCMAKE_TOOLCHAIN_FILE=/mnt/wdblue/vcpkg/scripts/buildsystems/vcpkg.cmake"
fi
if cmake -S "${ROOT}/core" -B "$tmpb" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release $toolchain >/dev/null 2>&1 \
   && cmake --build "$tmpb" -j"$(nproc)" >/dev/null 2>&1; then
    pass "core/ configures and builds as a project of its own"
else
    fail "roi_core does not build standalone"
fi

printf '\n'
if [[ $fails -eq 0 ]]; then
    printf 'all checks passed; rows appended to %s\n' "$CSV"
    exit 0
fi
printf '%d check(s) failed\n' "$fails"
exit 1
