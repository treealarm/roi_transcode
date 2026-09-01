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
# concurrency_test is here rather than only in ctest because it is the one test whose
# subject is a claim this file makes elsewhere: that separate encoders on separate threads
# are the supported way to use the library. It runs eight of them released at one instant
# and requires byte-identical output from each. Its real teeth are ThreadSanitizer --
# integration/scripts/tsan-check.sh, kept out of here because it needs its own instrumented
# build tree and costs an order of magnitude in time.
for t in object_normalizer_test object_geometry_test geometry_test public_api_test \
         concurrency_test; do
    if [[ -x "${LIBBUILD}/${t}" ]]; then
        if "${LIBBUILD}/${t}" >/dev/null 2>&1; then pass "$t"; else fail "$t"; fi
    else
        fail "$t not built"
    fi
done
for t in roi_json_test; do
    if [[ -x "${BUILD}/${t}" ]]; then
        if "${BUILD}/${t}" >/dev/null 2>&1; then pass "$t"; else fail "$t"; fi
    else
        fail "$t not built"
    fi
done

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

# The direct backends of §5.4, which until 2026-08-31 this script did not name once.
# That was the gap the x265direct crash walked through: it lived on a committed HEAD,
# through a green check.sh, because nothing here ever ran it. §5.4 is the headline of
# slices E and F and it deserves the same standard as the libavcodec paths.
#
# `no` for cqp: both run CRF, where rate control couples the regions, exactly as the
# libavcodec x265 path does.
step "the ROI mechanism does what it claims (x264 direct API, CRF, H.264)"
ENC=x264direct CODEC=h264 mechanism_checks x264direct no

step "the ROI mechanism does what it claims (x265 direct API, CRF, HEVC)"
ENC=x265direct CODEC=h265 mechanism_checks x265direct no

# Metadata whose size changes from frame to frame, which is a different test from the one
# above and the reason this step exists separately.
#
# testdata/roi.json holds its object for 0.25 s of a 12 s clip, so most frames carry no
# object and a few carry one: the SEI payload alternates between two lengths. The synth_*
# fixtures hold theirs for 20 s, so their payload never changes length -- and a payload
# that never changes length is precisely the input under which x265 4.1's user-SEI buffer
# reuse cannot fail. Verified against the pre-fix binary: it survives every synth_*
# fixture and segfaults on this one.
#
# So the assertion is not about quality here. It is that the run completes and every frame
# arrives, on a fixture where the metadata channel is not a constant.
# A short clip with one GOP over all of it, so nothing leaves the encoder until Flush and
# the drain has to hand back every buffered frame.
#
# This is the case that broke: x264direct on a 52-frame cut at --gop 50 wrote a 261-byte
# file with no pictures in it and exited 0. A flush call that returns no bytes does not
# mean the encoder is empty, and the drain was reading it that way. Neither the full
# 300-frame clip nor backend_conformance_test reproduces it -- both happen to get frames
# out before the flush -- so the short cut is the test.
step "direct backends drain what they buffered"
DRAIN_SRC="${WORK}/drain_src.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$SRC" -t 2 -c copy "$DRAIN_SRC"
drain_frames=$(count_packets "$DRAIN_SRC")
for _spec in "x264direct h264" "x265direct h265"; do
    set -- $_spec
    _enc="$1"; _codec="$2"
    if "${BUILD}/roi-transcode" --in "$DRAIN_SRC" --roi "${ROOT}/testdata/roi.json" \
        --out "${WORK}/drain_${_enc}.mp4" --encoder "$_enc" --codec "$_codec" \
        --crf 28 --gop 50 >"${WORK}/drain_${_enc}.log" 2>&1; then
        assert_eq "[${_enc}] every buffered frame comes out on flush" \
            "$(count_packets "${WORK}/drain_${_enc}.mp4")" "$drain_frames"
    else
        fail "[${_enc}] short-clip encode (rc=$?)"
    fi
done

step "direct backends survive metadata that changes size"
for _spec in "x264direct h264" "x265direct h265"; do
    set -- $_spec
    _enc="$1"; _codec="$2"
    if "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/roi.json" \
        --out "${WORK}/varmeta_${_enc}.mp4" --encoder "$_enc" --codec "$_codec" \
        --crf 28 --gop 15 >"${WORK}/varmeta_${_enc}.log" 2>&1; then
        pass "[${_enc}] encodes varying metadata without dying"
        assert_eq "[${_enc}] frame count kept" \
            "$(count_packets "${WORK}/varmeta_${_enc}.mp4")" "$src_frames"
        # Duration, not just the count -- and they are different questions. A packet
        # carries its own time base (packet_sink.h), both direct backends left it at the
        # 1/1 default until 2026-08-31, and the result was 300 frames muxed as 300 seconds
        # instead of 12. Every frame was present and every count assertion passed.
        assert_eq "[${_enc}] duration kept" \
            "$(probe format=duration "${WORK}/varmeta_${_enc}.mp4")" \
            "$(probe format=duration "$SRC")"
        assert_eq "[${_enc}] frame rate kept" \
            "$(probe stream=r_frame_rate "${WORK}/varmeta_${_enc}.mp4")" \
            "$(probe stream=r_frame_rate "$SRC")"
    else
        fail "[${_enc}] encode with varying metadata (rc=$?)"
        sed 's/^/      /' "${WORK}/varmeta_${_enc}.log" >&2
    fi
done

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

        # §5.2's native backend, on the same assertions as the libavcodec QSV path above.
        # `yes` for cqp: it opens CQP, so per-region offsets are independent per block and
        # equality of the background is a fair thing to assert.
        if "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
            --out "${WORK}/onevpl_probe.mp4" --encoder onevpldirect --codec h264 \
            --crf 28 --gop 1 >"${WORK}/onevpl_probe.log" 2>&1; then
            step "the ROI mechanism does what it claims (oneVPL direct, CQP, H.264)"
            ENC=onevpldirect CODEC=h264 mechanism_checks onevpl_h264 yes

            step "the ROI mechanism does what it claims (oneVPL direct, CQP, HEVC)"
            ENC=onevpldirect CODEC=h265 mechanism_checks onevpl_h265 yes
            # §2.2: block-level quality, and the one assertion that separates it from a
            # rectangle list. testdata/synth_manyroi.json holds six well-separated regions;
            # the iHD driver takes four rectangles on H.264, so on the rectangle path the
            # last two are truncated and stay in the crushed background. Through
            # mfxExtMBQP there is no rectangle budget to run out of and all six are the
            # same region.
            #
            # Asserted as the spread between the best-served and the worst-served of the
            # six, which is what "all six, equally" means and what a truncation destroys.
            # Measured 2026-09-01 on iHD 25.2: 0.00014 through the map, 0.01081 on the
            # rectangle path -- so the threshold below has two orders of magnitude of room
            # and still fails outright if the budget ever comes back.
            step "the block map serves every region, not the first four (§2.2)"
            if "${BUILD}/roi-transcode" --in "$SRC" \
                --roi "${ROOT}/testdata/synth_manyroi.json" \
                --out "${WORK}/mbqp_manyroi.mp4" --encoder onevpldirect --codec h264 \
                --crf 28 --gop 1 >"${WORK}/mbqp_manyroi.log" 2>&1; then
                # More regions than the driver takes as rectangles: the budget really is
                # gone upstream, rather than the map quietly carrying a truncated list.
                _rects=$(grep -oE 'ROI: [0-9]+ rects' "${WORK}/mbqp_manyroi.log" \
                    | head -1 | grep -oE '[0-9]+')
                assert_gt "the map carries more regions than H.264 takes as rectangles" \
                    "${_rects:-0}" "4"
                _lo=1; _hi=0
                for _xy in "64 57" "537 57" "998 57" "64 518" "537 518" "998 518"; do
                    # shellcheck disable=SC2086
                    set -- $_xy
                    _v=$(ssim "${WORK}/mbqp_manyroi.mp4" "$1" "$2" 204 86)
                    _lo=$(awk "BEGIN{print ($_v < $_lo) ? $_v : $_lo}")
                    _hi=$(awk "BEGIN{print ($_v > $_hi) ? $_v : $_hi}")
                done
                assert_lt "all six regions come out alike" \
                    "$(awk "BEGIN{printf \"%.6f\", $_hi - $_lo}")" "0.002"
            else
                fail "the block map run did not complete"
            fi
        else
            step "native oneVPL backend (§5.2)"
            printf '  skip  onevpldirect is not in this build or has no device\n'
        fi

        # The iHD driver takes four ROI regions for H.264 and eight for H.265. Both Intel
        # backends used to declare eight for both, and on the oneVPL path the surplus was
        # forwarded rather than trimmed -- iHD then overran a fixed-size array and the
        # process died inside vaEndPicture with no error status anywhere.
        #
        # This asserts the contract rather than the crash: the crash needs a region layout
        # this synthetic one does not reproduce, but the limits and the fact that hitting
        # them is said out loud are what stay true on every machine. §50 is the reason the
        # guard line is asserted too -- a truncation nobody can observe is a silent fallback.
        step "the Intel ROI region limit is per codec, and hitting it is audible"
        for _codec in h264 h265; do
            _log="${WORK}/manyroi_${_codec}.log"
            if "${BUILD}/roi-transcode" --in "${SRC}" --roi "${ROOT}/testdata/synth_manyroi.json" \
                --out "${WORK}/manyroi_${_codec}.mp4" --encoder qsv --codec "${_codec}" \
                --crf 28 >"${_log}" 2>&1; then
                _want=4; [[ "${_codec}" == h265 ]] && _want=8
                _got=$(grep -oE 'ROI: [0-9]+ rects' "${_log}" | head -1 | grep -oE '[0-9]+')
                assert_eq "qsv ${_codec} truncates to the driver's limit" "${_got:-none}" "${_want}"
                if grep -Eq "guard: [0-9]+ roi region\(s\) dropped; ${_codec} backend takes ${_want}" "${_log}"; then
                    pass "qsv ${_codec} says how many regions it dropped"
                else
                    fail "qsv ${_codec} dropped regions without saying so"
                fi
            else
                fail "qsv ${_codec} failed on a ROI set larger than the driver's limit"
            fi
        done
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
    #
    # x264, said plainly. This line read "--encoder x265" until 2026-08-31 and encoded
    # H.264 the whole time: the flag was an alias for the single CPU backend, and --codec
    # was left at its H.264 default. Now that x264 and x265 are separate backends the name
    # has to match what runs, which is what makes this the same test it always was.
    # Three points along the box's path: at each one the box's own patch must come out
    # better than a fixed patch of background. Echoes 1 when it does everywhere.
    moving_roi_follows() { # encoded-file
        local out="$1" ok=1
        for probe in "1.0 128 288" "6.0 550 288" "11.0 972 288"; do
            set -- $probe
            zone=$(ffmpeg -hide_banner -ss "$1" -i "${SCENES}/moving/source.mp4" \
                -ss "$1" -i "$out" -filter_complex \
                "[0:v]crop=384:144:$2:$3[a];[1:v]crop=384:144:$2:$3[b];[a][b]ssim" \
                -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2)
            back=$(ffmpeg -hide_banner -ss "$1" -i "${SCENES}/moving/source.mp4" \
                -ss "$1" -i "$out" -filter_complex \
                "[0:v]crop=384:144:40:40[a];[1:v]crop=384:144:40:40[b];[a][b]ssim" \
                -f null - 2>&1 | grep -oE 'All:[0-9.]+' | head -1 | cut -d: -f2)
            awk "BEGIN{exit !(${zone:-0} > ${back:-1})}" || ok=0
        done
        printf '%s' "$ok"
    }

    if "${BUILD}/roi-transcode" --in "${SCENES}/moving/source.mp4" \
        --roi "${SCENES}/moving/roi.json" --out "${WORK}/moving.mp4" \
        --encoder x264 --crf 28 --gop 1 >/dev/null 2>&1; then
        if [[ "$(moving_roi_follows "${WORK}/moving.mp4")" == 1 ]]; then
            pass "the ROI follows a moving box"
        else
            fail "the ROI did not follow the moving box"
        fi
    else
        fail "moving scene failed to encode"
    fi

    # The same box through the native oneVPL backend, and it is here for a reason the x264
    # line above does not carry: this is the only step in the file where the ROI *changes
    # between frames* on hardware. The block map and the ROI rectangles are handed to VPL
    # through buffers the runtime reads after EncodeFrameAsync returns (measured, срез O),
    # so a single set of them shared by every frame in flight is one frame overwriting
    # another frame's regions -- and a static fixture cannot see that, because every frame's
    # map is identical. All the other Intel steps use hold_sec 20.
    #
    # Weak teeth, said plainly: a shared buffer displaces the region by a few pixels per
    # frame, which barely moves SSIM over a 384x144 patch. It is what the harness can
    # measure at second granularity, and it is strictly more than the static fixtures see.
    if "${BUILD}/roi-transcode" --in "${SCENES}/moving/source.mp4" \
        --roi "${SCENES}/moving/roi.json" --out "${WORK}/moving_vpl.mp4" \
        --encoder onevpldirect --codec h264 --crf 28 --gop 1 >/dev/null 2>&1; then
        if [[ "$(moving_roi_follows "${WORK}/moving_vpl.mp4")" == 1 ]]; then
            pass "the ROI follows a moving box (oneVPL block map)"
        else
            fail "the ROI did not follow the moving box (oneVPL block map)"
        fi
    else
        printf '  skip  onevpldirect is not in this build or has no device\n'
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

    # The clip that actually reproduced the iHD abort. Its regions are tracked vehicles --
    # small, close together, and nothing synthetic here has provoked the driver the same
    # way -- so this is the only place the crash itself is under test rather than the
    # limit that avoids it. Optional, because the clip is not in the repository.
    if [[ -e /dev/dri/renderD128 ]] && source "${ROOT}/integration/scripts/qsv-env.sh" 2>/dev/null; then
        if "${BUILD}/roi-transcode" --in "${REAL_LAG_CLIP}" --roi "${REAL_LAG_ROI}" \
            --out "${WORK}/real_qsv_h264.mp4" --encoder qsv --codec h264 --crf 28 \
            >"${WORK}/real_qsv_h264.log" 2>&1; then
            pass "qsv h264 survives the real clip's region set"
        else
            fail "qsv h264 died on the real clip (rc=$?, see ${WORK}/real_qsv_h264.log)"
        fi
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

# §20 and §44, and the first place either can be shown with a number rather than asserted.
# It configures a shared build of §5.5's graph, installs it and reads what each object
# actually links -- so this step costs a Release build from scratch, and is worth it: until
# the components were split there was nothing for readelf to read, and "the core does not
# depend on FFmpeg" was a claim about intentions.
# §19.3 and §19.4. The line count is the easy half; what matters is that record N
# describes packet N, and the only way to get that wrong without it being obvious is
# B-frame reordering, where emission order is not submission order. Records written when a
# frame was handed in would pass a count and fail this.
# §23. Not just "it runs": the report has to agree with what the library actually does, and
# the Intel region limit is the sharpest place to check that. It is four for H.264 and
# eight for H.265 -- measured on this hardware, not read from a table (see STATUS.md) --
# and a report that flattened it to one number would be wrong about one of them. That
# difference is the reason this tool reports per codec at all, so it is what the step
# asserts.
step "sve-capabilities reports what the library can actually do"
CAPS_BIN="${LIBBUILD}/sve-capabilities"
if [[ ! -x "$CAPS_BIN" ]]; then
    fail "sve-capabilities not built (SVE_BUILD_TOOLS=ON)"
elif ! "$CAPS_BIN" --json > "${WORK}/caps.json" 2>"${WORK}/caps.log"; then
    fail "sve-capabilities --json"; sed 's/^/      /' "${WORK}/caps.log" >&2
else
    if python3 -c "
import json, sys
doc = json.load(open('${WORK}/caps.json'))
names = [b['backend'] for b in doc['backends']]
# The direct backends of §5.4 are in every build; their absence would mean the registry
# lost them rather than that the report is wrong.
assert 'x264direct' in names and 'x265direct' in names, names
for b in doc['backends']:
    for codec in b['codecs']:
        assert codec in b['capabilities'], (b['backend'], codec)
    if b['backend'] in ('qsv', 'vaapi') and set(b['codecs']) == {'h264', 'h265'}:
        h264 = b['capabilities']['h264']['max_roi_regions']
        h265 = b['capabilities']['h265']['max_roi_regions']
        assert (h264, h265) == (4, 8), (b['backend'], h264, h265)
" 2>"${WORK}/caps_check.log"; then
        pass "report is well formed and per-codec limits are the measured ones"
    else
        fail "sve-capabilities report disagrees with the library"
        sed 's/^/      /' "${WORK}/caps_check.log" >&2
    fi
    printf '  info  backends: %s\n' \
        "$(python3 -c "
import json
doc = json.load(open('${WORK}/caps.json'))
print(', '.join(b['backend'] + ('' if b['available'] else '(none)') for b in doc['backends']))")"
fi

step "sidecar records correspond to emitted packets"
if "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/roi.json" \
    --out "${WORK}/sidecar.mp4" --encoder x265direct --codec h265 --crf 30 --gop 15 \
    --sidecar >"${WORK}/sidecar.log" 2>&1; then
    meta="${WORK}/sidecar.svemeta.ndjson"
    if [[ -f "$meta" ]]; then
        assert_eq "one record per packet" \
            "$(wc -l < "$meta")" "$(count_packets "${WORK}/sidecar.mp4")"
        ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 \
            "${WORK}/sidecar.mp4" | tr -d ',' > "${WORK}/sidecar_pts.txt"
        if summary=$("${ROOT}/integration/scripts/sidecar-check.py" \
                "$meta" "${WORK}/sidecar_pts.txt" 2>&1); then
            pass "records line up with packets (${summary})"
        else
            fail "sidecar does not correspond to the packets: ${summary}"
        fi
        # Not a claim about the codec: a check that the check has teeth. On a stream with
        # no reordering, records written at submit would line up too.
        reordered=$(printf '%s' "$summary" | grep -oE '[0-9]+ reordering' | cut -d' ' -f1)
        assert_gt "the stream reorders, so order is really tested" "${reordered:-0}" "0"
    else
        fail "no sidecar was written"
    fi
else
    fail "sidecar encode"; sed 's/^/      /' "${WORK}/sidecar.log" >&2
fi

step "dependency scan: the core declares and links nothing it must not"
if depscan=$("${ROOT}/integration/scripts/dependency-scan.sh" "${WORK}/depscan" 2>&1); then
    printf '%s\n' "$depscan" | grep -E '^  (ok|skip)' | sed 's/^/  /'
    pass "$(printf '%s\n' "$depscan" | tail -1)"
else
    printf '%s\n' "$depscan" | grep -E '^  (FAIL|skip)' | sed 's/^/  /' >&2
    fail "dependency scan (§26.4, §44)"
fi

# §58.9's Python suite. It needs its own build directory: a Python extension is a shared
# object, and the libavcodec-backed components this build uses link a static FFmpeg without
# -fPIC. The configuration below is §5.5's graph, which is what §58.9 asks the tests to run
# against anyway.
#
# Skipped loudly when the toolchain is not there. A skip is not a pass, and this one is
# printed rather than hidden -- the same hazard CLAUDE.md records for the synthetic scenes.
step "python bindings (§58)"
if ! python3 -c "import pybind11, numpy, pytest" >/dev/null 2>&1; then
    printf '  skip  python bindings need pybind11, numpy and pytest\n'
elif ! python3-config --includes >/dev/null 2>&1; then
    printf '  skip  python bindings need the CPython development headers\n'
else
    PYBUILD="${WORK}/python-build"
    if cmake -S "$ROOT" -B "$PYBUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-/mnt/wdblue/vcpkg}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_MODE=OFF -DCMAKE_BUILD_TYPE=Release \
        -DSVE_BUILD_PYTHON=ON -DSVE_BUILD_TESTS=OFF -DSVE_BUILD_TOOLS=OFF \
        -DSVE_BUILD_INTEGRATION=OFF -DSVE_BUILD_BACKEND_FFMPEG=OFF \
        -DSVE_BUILD_EXAMPLES=OFF >"${WORK}/python_build.log" 2>&1 \
        && cmake --build "$PYBUILD" -j"$(nproc)" >>"${WORK}/python_build.log" 2>&1; then
        if pyout=$(cd "${ROOT}/python" && python3 -m pytest tests/ -q 2>&1); then
            pass "pytest: $(printf '%s' "$pyout" | tail -1)"
        else
            fail "python bindings suite"
            printf '%s\n' "$pyout" | tail -20 | sed 's/^/      /' >&2
        fi
        # §58.7 and §58.9's wheel-install test. A wheel that builds is not a wheel that
        # works: the module has to import with no build tree in sight, which is the one
        # thing running pytest inside python/ cannot show.
        if python3 -c "import build, scikit_build_core" >/dev/null 2>&1; then
            WHEELDIR="${WORK}/wheel"
            # The toolchain has to reach the wheel build: it configures its own CMake in
            # an isolated environment, and without vcpkg pkg_check_modules cannot find
            # FFmpeg. CMake reads CMAKE_TOOLCHAIN_FILE from the environment since 3.21.
            if (cd "${ROOT}/python" \
                && VCPKG_ROOT="${VCPKG_ROOT:-/mnt/wdblue/vcpkg}" \
                   CMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-/mnt/wdblue/vcpkg}/scripts/buildsystems/vcpkg.cmake" \
                   python3 -m build --wheel --outdir "$WHEELDIR" . ) \
                    >"${WORK}/wheel.log" 2>&1; then
                whl=$(ls "${WHEELDIR}"/*.whl 2>/dev/null | head -1)
                # Only the package and its extension belong in it. Headers and static
                # archives are unusable from Python and were 1.5 MB of nothing until
                # SVE_INSTALL=OFF; this is what keeps that from coming back.
                stray=$(python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
print(' '.join(sorted({n.split('/')[0] for n in names}
                      - {'smart_video_encoder', 'smart_video_encoder-2.0.0.dist-info'})))
" "$whl" 2>/dev/null)
                if [[ -z "${stray// }" ]]; then
                    pass "wheel carries only the package ($(basename "$whl"))"
                else
                    fail "wheel carries more than the package: ${stray}"
                fi
                site="${WORK}/wheel-site"
                rm -rf "$site"
                if pip install --quiet --no-index --no-deps --target "$site" "$whl" \
                        >>"${WORK}/wheel.log" 2>&1 \
                    && (cd "$WORK" && PYTHONPATH="$site" python3 -c "
import smart_video_encoder as sve
assert sve.__version__, 'no version'
assert sve.build_manifest['commit'], 'no build manifest'
assert sve.available_backends(), 'no backends in the wheel'
") >>"${WORK}/wheel.log" 2>&1; then
                    pass "wheel installs and imports with no build tree"
                else
                    fail "wheel install/import"
                    tail -10 "${WORK}/wheel.log" | sed 's/^/      /' >&2
                fi
            else
                fail "wheel build"
                tail -10 "${WORK}/wheel.log" | sed 's/^/      /' >&2
            fi
        else
            printf '  skip  wheel test needs build and scikit-build-core\n'
        fi
    else
        fail "python extension build"
        tail -15 "${WORK}/python_build.log" | sed 's/^/      /' >&2
    fi
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
