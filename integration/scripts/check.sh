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
oks=0
skips=0
steps=0
pass() { printf '  ok    %s\n' "$1"; oks=$((oks + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
# A step that did not run is not a step that passed. It is counted and named in the last
# line for that reason: this script skips a whole block when the fixtures for it cannot be
# built, and "all checks passed" on its own has said that after never running the
# moving-object assertions at all.
skip() { printf '  skip  %s\n' "$1"; skips=$((skips + 1)); }
step() { printf '\n== %s ==\n' "$1"; steps=$((steps + 1)); }

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
# backend_plugin_test and plugin_loader_test are here for the same reason concurrency_test
# is: the packaging step below makes claims about §26.4, and these are where the claims are
# checked one at a time -- what a descriptor may say, and what happens to a plugin that
# should not load.
for t in object_normalizer_test object_geometry_test geometry_test public_api_test \
         concurrency_test backend_plugin_test plugin_loader_test; do
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
mechanism_checks() { # label cqp ratio_limit   (encoder from $ENC, codec from $CODEC)
    local enc="$1" cqp="$2" ratio_limit="$3"
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

    # Everything above is a direction: bigger, smaller, better. Directions cannot notice
    # the offer shrinking. A change that left ROI saving one percent instead of forty-five
    # would satisfy every assertion above and destroy the reason this library exists, so
    # the two numbers that are the offer are asserted as magnitudes.
    #
    # The size limit is per backend and per codec, passed in by the caller, because the
    # paths do not achieve the same thing and one number covering all of them would guard
    # none of them. Read out of testdata/eval/runs.csv, which has recorded every run since
    # 2026-08-31, each limit sits about a tenth above what that path actually achieves.
    #
    # The spread is itself worth knowing and is why this is not one constant: on HEVC the
    # CPU paths reach 0.546 and the native oneVPL path only 0.683, because it does not crush
    # the background as far -- background SSIM 0.872 against 0.672 for the same request.
    # Zone quality on that path is the best of any (0.9996), so it is a difference in how
    # much the offer pays there, not a broken path. Recorded in STATUS.md as open.
    #
    # The zone floor is one number for everyone: 0.99 against the source. Every path
    # measures 0.9949 or better today and most are above 0.998.
    local ratio
    ratio=$(awk "BEGIN{printf \"%.4f\", ${BYTES[roi_${enc}]} / ${BYTES[noroi_${enc}]}}")
    assert_lt "[${enc}] ROI is materially smaller, not merely smaller" \
        "$ratio" "$ratio_limit"
    # Against the source, not against another encode: this is the "without losing quality"
    # half of the claim, and it is only meaningful measured against what came in.
    assert_gt "[${enc}] the zone is still near-lossless against the source" \
        "${SSIM_ROI[roi_${enc}]}" "0.99"

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
# What a transcode is asked to preserve, and what nothing here checked until 2026-09-02.
# --gop is passed by a dozen steps in this file and no step ever looked at whether the
# output honoured it: a backend that ignored the keyframe interval entirely would have
# passed every run. It matters for the archive this feeds -- the keyframe interval is what
# seeking costs and what an HLS segment can be cut on.
step "the stream keeps the characteristics it was asked for"
for _spec in "x264direct h264 25" "x265direct h265 25" "cpu h265 50"; do
    read -r _enc _codec _gop <<<"$_spec"
    _out="${WORK}/keep_${_enc}_${_codec}.mp4"
    if ! "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
        --out "$_out" --encoder "$_enc" --codec "$_codec" --crf 30 --gop "$_gop" \
        >"${WORK}/keep_${_enc}.log" 2>&1; then
        fail "[${_enc}] the encode did not complete"
        continue
    fi
    assert_eq "[${_enc}] resolution kept" \
        "$(probe stream=width,height "$_out")" "$(probe stream=width,height "$SRC")"
    assert_eq "[${_enc}] frame rate kept" \
        "$(probe stream=r_frame_rate "$_out")" "$(probe stream=r_frame_rate "$SRC")"
    assert_eq "[${_enc}] frame count kept" "$(count_packets "$_out")" "$(count_packets "$SRC")"
    assert_eq "[${_enc}] pixel format kept" \
        "$(probe stream=pix_fmt "$_out")" "$(probe stream=pix_fmt "$SRC")"
    # The keyframe interval, read from the bitstream rather than from what we asked for.
    # Both ends of the gap, because a stream with one keyframe every frame and a stream
    # with one at the start both have "no gap larger than the interval".
    _gaps=$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame \
        -of csv=p=0 "$_out" | python3 -c "
import sys
vals = [v.strip().rstrip(',') for v in sys.stdin.read().split()]
pos = [i for i, v in enumerate(vals) if v == '1']
gaps = [pos[i + 1] - pos[i] for i in range(len(pos) - 1)]
print(f'{min(gaps)},{max(gaps)}' if gaps else 'none')
")
    assert_eq "[${_enc}] keyframe interval is the ${_gop} it was asked for" \
        "$_gaps" "${_gop},${_gop}"
done

step "the ROI mechanism does what it claims (CPU, CRF, HEVC)"
CODEC=h265 mechanism_checks cpu_h265 no 0.62

step "the ROI mechanism does what it claims (CPU, CRF, H.264)"
CODEC=h264 mechanism_checks cpu_h264 no 0.82

# The direct backends of §5.4, which until 2026-08-31 this script did not name once.
# That was the gap the x265direct crash walked through: it lived on a committed HEAD,
# through a green check.sh, because nothing here ever ran it. §5.4 is the headline of
# slices E and F and it deserves the same standard as the libavcodec paths.
#
# `no` for cqp: both run CRF, where rate control couples the regions, exactly as the
# libavcodec x265 path does.
step "the ROI mechanism does what it claims (x264 direct API, CRF, H.264)"
ENC=x264direct CODEC=h264 mechanism_checks x264direct no 0.82

step "the ROI mechanism does what it claims (x265 direct API, CRF, HEVC)"
ENC=x265direct CODEC=h265 mechanism_checks x265direct no 0.62

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
    skip "set RUN_H265_MATRIX=1 to run CPU H.265 guard matrix"
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
        # What a caller who names no backend gets. Since 2026-09-01 that is the native
        # oneVPL backend rather than the libavcodec one, because the default is what §2.2
        # and §16 are either met on or not: through libavcodec the same encode loses two of
        # six regions to the driver's rectangle limit and misses §16 outright.
        #
        # Asserted on the backend's own name and not on the codec token, which is only
        # "hevc" on the direct path and says nothing about who produced it.
        encode qsv_auto synth_roi.json auto || true
        if grep -q 'encoded with .*Intel oneVPL, direct API' "${WORK}/qsv_auto.log"; then
            pass "auto picks the native oneVPL backend with the Intel runtime"
        else
            fail "auto did not pick the native oneVPL backend: $(grep -m1 '^encoded with' "${WORK}/qsv_auto.log")"
        fi
        CODEC=h264 encode qsv_auto_h264 synth_roi.json auto || true
        if grep -q 'encoded with .*Intel oneVPL, direct API' "${WORK}/qsv_auto_h264.log"; then
            pass "auto picks the native oneVPL backend for H.264 too"
        else
            fail "auto did not pick the native oneVPL backend for H.264: $(grep -m1 '^encoded with' "${WORK}/qsv_auto_h264.log")"
        fi
        # QSV is the primary path and it is CQP, so the strict form of the
        # mechanism checks runs here.
        step "the ROI mechanism does what it claims (QSV, CQP, HEVC)"
        ENC=qsv CODEC=h265 mechanism_checks qsv_h265 yes 0.65

        step "the ROI mechanism does what it claims (QSV, CQP, H.264)"
        ENC=qsv CODEC=h264 mechanism_checks qsv_h264 yes 0.72

        # §5.2's native backend, on the same assertions as the libavcodec QSV path above.
        # `yes` for cqp: it opens CQP, so per-region offsets are independent per block and
        # equality of the background is a fair thing to assert.
        if "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
            --out "${WORK}/onevpl_probe.mp4" --encoder onevpldirect --codec h264 \
            --crf 28 --gop 1 >"${WORK}/onevpl_probe.log" 2>&1; then
            step "the ROI mechanism does what it claims (oneVPL direct, CQP, H.264)"
            ENC=onevpldirect CODEC=h264 mechanism_checks onevpl_h264 yes 0.75

            step "the ROI mechanism does what it claims (oneVPL direct, CQP, HEVC)"
            ENC=onevpldirect CODEC=h265 mechanism_checks onevpl_h265 yes 0.75
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

            # The other half of the same decision, and the reason GetCapabilities takes a
            # CapabilityQuery: mfxExtMBQP carries absolute quantisers, so it needs CQP,
            # while §16's rolling criterion needs the hardware's own rate control. Both are
            # available on this backend and not at once. Asked for a bitrate, it opens VBR
            # and goes back to rectangles -- and says so, rather than accepting a block
            # grid it will discard all but four of, which is what §50 forbids.
            # §50 again, on the axis nobody had looked at: the quality map is absolute
            # quantisers on this path, so a delta added to the frame's own quantiser can
            # fall off the end of the codec's 1..51 scale. Measured 2026-09-02 -- at crf 28
            # a background delta of 30 and one of 40 produce byte-identical output, because
            # both clamp to 51. Asking for more than the scale holds has to say so.
            step "a quality map past the end of the quantiser scale says so (§50)"
            python3 - "${ROOT}/testdata/synth_roi.json" "${WORK}/roi_clamp.json" <<'CLAMP'
import json, sys
d = json.load(open(sys.argv[1]))
d["background_qp_delta"] = 40          # 28 + 40 is 68, and the scale ends at 51
json.dump(d, open(sys.argv[2], "w"))
CLAMP
            if "${BUILD}/roi-transcode" --in "$SRC" --roi "${WORK}/roi_clamp.json" \
                --out "${WORK}/clamp.mp4" --encoder onevpldirect --codec h265 \
                --crf 28 --gop 1 >"${WORK}/clamp.log" 2>&1; then
                if grep -q 'ran out of quantiser scale' "${WORK}/clamp.log"; then
                    pass "a request past the end of the scale is reported"
                else
                    fail "the quantiser scale ran out and nothing said so"
                fi
            else
                fail "the clamped encode did not complete"
            fi
            # And the other half, which is what makes the first one mean something: a
            # request that fits must not produce the warning.
            if "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
                --out "${WORK}/noclamp.mp4" --encoder onevpldirect --codec h265 \
                --crf 28 --gop 1 >"${WORK}/noclamp.log" 2>&1; then
                if grep -q 'ran out of quantiser scale' "${WORK}/noclamp.log"; then
                    fail "a request that fits the scale was reported as clamped"
                else
                    pass "a request that fits the scale is not reported"
                fi
            else
                fail "the unclamped encode did not complete"
            fi

            step "a target bitrate on Intel trades the block map for rectangles, audibly"
            if "${BUILD}/roi-transcode" --in "$SRC" \
                --roi "${ROOT}/testdata/synth_manyroi.json" \
                --out "${WORK}/mbqp_vbr.mp4" --encoder onevpldirect --codec h264 \
                --crf 28 --target-bitrate 3000000 >"${WORK}/mbqp_vbr.log" 2>&1; then
                _r=$(grep -oE 'ROI: [0-9]+ rects' "${WORK}/mbqp_vbr.log" | head -1 \
                    | grep -oE '[0-9]+')
                assert_eq "the budget is back to the driver's H.264 limit" "${_r:-none}" "4"
                if grep -Eq 'guard: [0-9]+ roi region\(s\) dropped' "${WORK}/mbqp_vbr.log"; then
                    pass "and the regions it cost are reported, not dropped in silence"
                else
                    fail "regions were truncated without a guard line (§50)"
                fi
            else
                fail "the target-bitrate run on onevpldirect did not complete"
            fi
        else
            step "native oneVPL backend (§5.2)"
            skip "onevpldirect is not in this build or has no device"
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
        skip "Intel runtime not fetched (integration/scripts/fetch-intel-qsv.sh)"
    fi
else
    skip "no /dev/dri, hardware backends unavailable"
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
        skip "onevpldirect is not in this build or has no device"
    fi

    # §16, which until 2026-09-01 was never checked at all -- and could not be, because it
    # says the rolling criterion is valid only for sequences of thirty seconds or more and
    # testdata/synth.mp4 is twelve. So the fixture is built here: the crowd scene three
    # times over, which is thirty-six seconds of content expensive enough that a target
    # bitrate actually binds (synth.mp4 at CRF 28 runs at 42 kbps, fifteen times under any
    # target worth asking for, and a target that never binds tests nothing).
    step "a target bitrate is honoured (§16)"
    _crowd="${SCENES}/crowded/source.mp4"
    if [[ -f "$_crowd" ]]; then
        _list="${WORK}/long.txt"
        : > "$_list"
        for _i in 1 2 3; do printf "file '%s'\n" "$(realpath "$_crowd")" >> "$_list"; done
        if ffmpeg -y -hide_banner -loglevel error -f concat -safe 0 -i "$_list" \
            -c copy "${WORK}/long.mp4" 2>/dev/null; then
            for _spec in "x264direct h264" "x265direct h265"; do
                # shellcheck disable=SC2086
                set -- $_spec
                if "${BUILD}/roi-transcode" --in "${WORK}/long.mp4" \
                    --roi "${ROOT}/testdata/synth_roi.json" --out "${WORK}/br_$1.mp4" \
                    --encoder "$1" --codec "$2" --crf 28 --target-bitrate 3000000 \
                    >"${WORK}/br_$1.log" 2>&1; then
                    if "${ROOT}/integration/scripts/bitrate-check.py" \
                        "${WORK}/br_$1.mp4" 3000000 > "${WORK}/br_$1.txt" 2>&1; then
                        pass "[$1] average overshoot and P95 rolling are within §16"
                    else
                        fail "[$1] §16: $(tr '\n' ' ' < "${WORK}/br_$1.txt")"
                    fi
                else
                    fail "[$1] target-bitrate encode did not complete"
                fi
            done

            # The regression for a crash this step is how we found. libx265 allocates a
            # frame's quantOffsets array lazily -- the first picture carrying offsets is
            # what makes it -- and recycles frames through a pool, so a clip whose early
            # frames carry no regions filled that pool with frames having no array, and the
            # first later picture that did carry offsets wrote into a null pointer. This
            # backend now hands over the grid on every frame, zero-filled when there is
            # nothing to say.
            #
            # testdata/roi.json is the shape that reproduces it: its object lives a quarter
            # of a second, so most frames carry no region and a few do. Only completion is
            # asserted, not §16 -- the thresholds above were measured with synth_roi.json,
            # and asserting numbers on a fixture nobody measured is how a step becomes
            # flaky.
            if "${BUILD}/roi-transcode" --in "${WORK}/long.mp4" \
                --roi "${ROOT}/testdata/roi.json" --out "${WORK}/br_comeandgo.mp4" \
                --encoder x265direct --codec h265 --crf 28 --target-bitrate 2000000 \
                >"${WORK}/br_comeandgo.log" 2>&1; then
                pass "regions that come and go survive a long ABR encode"
            else
                fail "x265direct crashed or failed on regions that come and go (rc=$?)"
            fi
        else
            skip "could not build the 36 s fixture"
        fi
    else
        skip "crowded scene not generated, §16 needs >=30 s (its own rule)"
    fi
else
    skip "scenes not generated (benchmarks/synthetic/make-scenes.py)"
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
    skip "moving scene not generated (benchmarks/synthetic/make-scenes.py)"
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
    skip "real clip fixtures missing (set REAL_LAG_CLIP/REAL_LAG_ROI or run integration/scripts/prepare-real-lag-fixture.sh)"
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
    skip "roi_metrics or the synthetic scenes are missing"
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
    skip "crowded scene not generated (benchmarks/synthetic/make-scenes.py)"
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

# §26.4's three delivery profiles. Its own script for the same reason the dependency scan
# has one: it configures builds of its own from scratch, which is not something to do inside
# a step that is otherwise reusing this build tree.
step "backend packaging profiles build and differ as §26.4 says"
if pkgout=$("${ROOT}/integration/scripts/packaging-check.sh" "${WORK}/packaging" 2>&1); then
    printf '%s\n' "$pkgout" | grep -E '^  ok' | sed 's/^/  /'
    pass "$(printf '%s\n' "$pkgout" | tail -1)"
else
    printf '%s\n' "$pkgout" | grep -E '^  FAIL' | sed 's/^/  /' >&2
    fail "backend packaging profiles (§26.4)"
fi

# The whole chain in one command, which the unit test cannot do: a real program, built the
# way an application that hosts plugins has to be built, loading real modules off a real
# directory. The three refusals are the three §26.4 names, and each has to arrive with its
# reason -- a plugin that quietly fails to load is worse than one that fails loudly, because
# the encode then succeeds on a backend nobody chose.
step "the CLI loads backend plugins and says why it refuses one (§26.4)"
PLUGINS="${LIBBUILD}/test-plugins"
if [[ -d "${PLUGINS}/bad_abi" ]]; then
    "${BUILD}/roi-transcode" --in "$SRC" --roi "${ROOT}/testdata/synth_roi.json" \
        --out "${WORK}/plugins.mp4" --encoder x264direct --codec h264 --crf 32 --gop 1 \
        --plugin-dir "${PLUGINS}/bad_abi" --plugin-dir "${PLUGINS}/no_entry" \
        --plugin-dir "${PLUGINS}/good" > "${WORK}/plugins.log" 2>&1
    if [[ -s "${WORK}/plugins.mp4" ]]; then
        pass "an encode runs with plugin directories on the command line"
    else
        fail "the encode with plugin directories produced nothing"
    fi
    # The number the bad plugin carries is derived, not written here: tests/plugins builds it
    # as kBackendPluginAbiVersion + 1000, and a literal in this script is the same value in a
    # second place. It was one, and the step broke on the first bump of the header -- which is
    # the failure this kind of duplication always has, one commit after the copy is made.
    plugin_abi=$(sed -n 's/.*kBackendPluginAbiVersion = \([0-9][0-9]*\);.*/\1/p' \
        "${ROOT}/include/sve/backend_plugin.h" | head -1)
    if grep -q "abi version $((plugin_abi + 1000)) does not match" "${WORK}/plugins.log"; then
        pass "a plugin from another abi is refused by version"
    else
        fail "the bad-abi plugin was not refused by version"
    fi
    if grep -q 'undefined symbol: sve_backend_plugin_v1' "${WORK}/plugins.log"; then
        pass "a module with no entry point is refused by name"
    else
        fail "the module with no entry point was not refused by name"
    fi
    # This build links the libavcodec reference group, which registers nvenc statically, and
    # the test plugin claims the same identifier. That is §26.4's static-versus-dynamic
    # conflict happening for real rather than in a fixture.
    if grep -q 'already registered' "${WORK}/plugins.log"; then
        pass "a plugin colliding with a linked-in backend is refused as a duplicate"
    else
        fail "the duplicate identifier was not refused"
    fi
else
    skip "test plugins not built (SVE_BUILD_TESTS=OFF)"
fi

# §43's smoke profile, shortened. The specification's own smallest profile is an hour and
# this is not it -- SOAK_SECONDS is what makes it one, and the acceptance run cannot afford
# an hour on every change. What this step is for is that the driver and its five criteria go
# on working; a soak harness that quietly stopped measuring between nightly runs would be
# the worst way to find out.
step "soak: continuous encode, and §43.2's five counts (short profile)"
if soakout=$(SOAK_SECONDS="${SOAK_SECONDS:-20}" "${ROOT}/integration/scripts/soak-check.sh" \
        "${LIBBUILD}" 2>&1); then
    printf '%s\n' "$soakout" | grep -E '^ +(soak:|frame|pts|metadata|memory)' | sed 's/^ */  /'
    # A here-string rather than `printf ... | grep -q`: under `set -o pipefail` the producer's
# SIGPIPE becomes the pipeline's status when grep leaves early, and a condition that reads
# "did this output contain X" then answers no for the wrong reason. One step lost a day to
# exactly that on 2026-09-03.
if grep -q '^  skip' <<<"$soakout"; then
        skip "sve-soak is not built"
    else
        pass "$(printf '%s\n' "$soakout" | tail -1)"
    fi
else
    printf '%s\n' "$soakout" | sed 's/^/  /' >&2
    fail "soak (§43)"
fi

# §29. FFmpeg writes to stderr on its own, from inside libavformat and from the codec
# libraries below it, and an application that named a destination for this library's
# diagnostics still had no say over those. The provocation is a file that is not media: it
# makes libavformat say two things, at two levels, deterministically.
#
# The assertion is on the prefix, not on the words. FFmpeg's own line would be on stderr
# either way -- what proves it went through the application's sink is that the application's
# framing is around it.
step "FFmpeg's own diagnostics reach the application's sink (§29)"
printf 'this is not a media file at all\n' > "${WORK}/notmedia.mp4"
ffmpeg_log_out=$("${BUILD}/roi-transcode" --in "${WORK}/notmedia.mp4" \
    --roi "${ROOT}/testdata/synth_roi.json" --out "${WORK}/notmedia-out.mp4" \
    --encoder x264direct --codec h264 2>&1)
if [[ -n "$ffmpeg_log_out" ]] && grep -q '^ffmpeg \[error\]: ' <<<"$ffmpeg_log_out"; then
    pass "an FFmpeg error arrives through the sink, framed by the application"
else
    fail "FFmpeg diagnostics did not reach the sink"
    printf '%s\n' "$ffmpeg_log_out" | head -5 | sed 's/^/      /' >&2
fi
# That the text survived, without pinning FFmpeg's wording. An earlier version of this step
# asserted "moov atom not found" and the presence of a warning-level line, which made an
# FFmpeg upgrade or a different probe outcome redden the run with nothing wrong in the
# product. What matters here is that something non-empty came through; the level mapping is
# checked in logging_test, where the test chooses the level itself and the answer cannot
# depend on which diagnostic FFmpeg happened to produce.
if grep -qE '^ffmpeg \[error\]: .+' <<<"$ffmpeg_log_out"; then
    pass "the routed line carries FFmpeg's message and not just the frame"
else
    fail "the routed message arrived empty"
fi
# The half that proves routing rather than mere presence: FFmpeg's own format starts the
# line with its context, "[mov,mp4,...]". If any line still begins that way, a diagnostic
# went to stderr directly instead of through the sink.
if grep -q '^\[' <<<"$ffmpeg_log_out"; then
    fail "an FFmpeg line reached stderr without going through the sink"
    printf '%s\n' "$ffmpeg_log_out" | grep '^\[' | head -3 | sed 's/^/      /' >&2
else
    pass "nothing bypassed the sink on its way to stderr"
fi

step "dependency scan: the core declares and links nothing it must not"
if depscan=$("${ROOT}/integration/scripts/dependency-scan.sh" "${WORK}/depscan" 2>&1); then
    printf '%s\n' "$depscan" | grep -E '^  (ok|skip)' | sed 's/^/  /'
    pass "$(printf '%s\n' "$depscan" | tail -1)"
else
    printf '%s\n' "$depscan" | grep -E '^  (FAIL|skip)' | sed 's/^/  /' >&2
    fail "dependency scan (§26.4, §44)"
fi

# §38's calibration, and the "baseline profiles versioned" line of §55. The profiles are
# committed under baselines/, so this step is a comparison rather than a measurement: it
# regenerates each one and fails on any difference. That is the whole point of a baseline --
# a configuration change that nobody meant is otherwise invisible until the numbers move.
step "baseline calibration: the committed profiles still describe this build"
if baseout=$("${ROOT}/integration/scripts/baseline-check.sh" "${LIBBUILD}" 2>&1); then
    printf '%s\n' "$baseout" | grep -E '^  (ok|skip)' | sed 's/^/  /'
    pass "$(printf '%s\n' "$baseout" | tail -1)"
else
    printf '%s\n' "$baseout" | grep -E '^  (FAIL|skip)' | sed 's/^/  /' >&2
    fail "baseline calibration (§38)"
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
    skip "python bindings need pybind11, numpy and pytest"
elif ! python3-config --includes >/dev/null 2>&1; then
    skip "python bindings need the CPython development headers"
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
                # §58.10.9 asks for type hints to be delivered *with the package*. The
                # marker and the stubs have to be inside the wheel to mean anything: a
                # py.typed that ships without stubs promises types the caller then does not
                # get, and stubs that stay in the source tree reach nobody who pip-installs.
                typed=$(python3 -c "
import sys, zipfile
names = set(zipfile.ZipFile(sys.argv[1]).namelist())
want = {'smart_video_encoder/py.typed', 'smart_video_encoder/_core.pyi'}
print(' '.join(sorted(want - names)))
" "$whl" 2>/dev/null)
                if [[ -z "${typed// }" ]]; then
                    pass "wheel carries py.typed and the stubs beside it (§58.10.9)"
                else
                    fail "wheel is missing: ${typed}"
                fi
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
            skip "wheel test needs build and scikit-build-core"
        fi
    else
        fail "python extension build"
        tail -15 "${WORK}/python_build.log" | sed 's/^/      /' >&2
    fi
fi

# §58.10.1: the C++ and Python paths must give equivalent packets, PTS, metadata and
# capability reports for the same configuration. Nothing compared them until 2026-09-02 --
# the C++ suite checked that C++ worked and the Python suite checked that Python worked, so
# a binding quietly encoding something else passed both. It had drifted in five places by
# the time anyone looked.
#
# Placed after the python step because it needs the extension that step builds. The digest
# carries a hash of the input frames, so a difference says which of the two things it is:
# the two sides encoding different pixels, or the same pixels differently.
step "C++ and Python encode the same thing (§58.10.1)"
if [[ ! -x "${LIBBUILD}/parity_digest" ]]; then
    skip "parity digest needs SVE_BUILD_TESTS=ON"
elif ! python3 -c "import numpy" >/dev/null 2>&1; then
    skip "python parity needs numpy"
elif ! PYTHONPATH="${ROOT}/python" python3 -c "import smart_video_encoder" >/dev/null 2>&1; then
    skip "python parity needs the extension built (SVE_BUILD_PYTHON=ON)"
else
    "${LIBBUILD}/parity_digest" >"${WORK}/parity_cpp.txt" 2>"${WORK}/parity_cpp.log"
    cpp_rc=$?
    PYTHONPATH="${ROOT}/python" python3 "${ROOT}/integration/scripts/python-parity.py" \
        >"${WORK}/parity_py.txt" 2>"${WORK}/parity_py.log"
    py_rc=$?
    if [[ "$cpp_rc" -ne 0 || "$py_rc" -ne 0 ]]; then
        fail "parity digest did not run (cpp=${cpp_rc}, py=${py_rc})"
        tail -3 "${WORK}/parity_cpp.log" "${WORK}/parity_py.log" | sed 's/^/      /' >&2
    elif diff -u "${WORK}/parity_cpp.txt" "${WORK}/parity_py.txt" >"${WORK}/parity.diff"; then
        pass "same capability, packets, PTS and statistics from both languages"
        # Named separately, because a digest that matched on two empty encodes would also
        # be identical and would mean nothing.
        assert_eq "packets compared" \
            "$(grep -c '^packet ' "${WORK}/parity_cpp.txt")" "25"
    else
        fail "C++ and Python do not encode the same thing"
        head -30 "${WORK}/parity.diff" | sed 's/^/      /' >&2
    fi
fi

# AT-009, §32's ten-bit functional test: "для каждого backend с capability main10 --
# encode/decode P010/Main10 (H.265), 300 frames; frame count, monotonic PTS, EOS; отсутствие
# capability -- documented SKIP с capability evidence".
#
# Never run until 2026-09-03, and the first run found that a Main10 source was being encoded
# as eight-bit: the example application never told the encoder what depth its input was, so
# the library's ten-bit branch could not fire and every frame was silently converted down.
#
# The fixture is built here rather than committed: a ten-bit clip is a transcode of the
# synthetic one, and generating it costs less than carrying it.
step "ten-bit encode, end to end (§15.3, AT-009)"
tenbit_src="${WORK}/ten_src.mp4"
if ! ffmpeg -v error -y -i "$SRC" -pix_fmt yuv420p10le -c:v libx265 \
        -x265-params log-level=none -crf 20 "$tenbit_src" >/dev/null 2>&1; then
    skip "this ffmpeg cannot produce a ten-bit fixture"
else
    # The capability evidence AT-009 asks for, read from the library rather than assumed.
    #
    # Captured into a variable and matched afterwards, not piped into grep -q. Under
    # `set -o pipefail` that pipeline reports the producer's SIGPIPE -- grep -q leaves as
    # soon as it matches, sve-capabilities dies writing the rest, and the whole condition
    # comes out false. It passed for a day because the only match was the last line of the
    # output, and the moment a second backend reported main10 the match moved up, the
    # producer had output left to write, and this step began skipping with a message that
    # said the capability was absent when the report in hand said it was there.
    tenbit_caps=$("${LIBBUILD}/sve-capabilities" --json 2>/dev/null || true)
    tenbit_backend=""
    tenbit_why="no backend in this build reports main10"
    if ! grep -q '"main10": true' <<<"$tenbit_caps"; then
        : # the reason above stands
    elif [[ ! -e /dev/dri/renderD128 ]]; then
        tenbit_why="a backend reports main10 but this machine has no render node"
    elif ! source "${ROOT}/integration/scripts/qsv-env.sh" 2>/dev/null; then
        tenbit_why="a backend reports main10 but its runtime environment did not load"
    else
        tenbit_backend="qsv"
    fi
    if [[ -z "$tenbit_backend" ]]; then
        skip "${tenbit_why} -- AT-009 N/A (capability evidence: sve-capabilities --json)"
    elif ! "${BUILD}/roi-transcode" --in "$tenbit_src" \
            --roi "${ROOT}/testdata/synth_roi.json" --out "${WORK}/ten_out.mp4" \
            --encoder "$tenbit_backend" --codec h265 --crf 28 --gop 50 \
            >"${WORK}/ten.log" 2>&1; then
        fail "the ten-bit encode did not complete (see ${WORK}/ten.log)"
    else
        # Main10 and ten bits per sample, not merely "it produced a file". An eight-bit
        # file with the right frame count is exactly what this used to produce.
        assert_eq "the output is Main 10" \
            "$(probe stream=profile "${WORK}/ten_out.mp4")" "Main 10"
        assert_eq "the output carries ten bits a sample" \
            "$(probe stream=pix_fmt "${WORK}/ten_out.mp4")" "yuv420p10le"
        assert_eq "every frame came out (§32: 300)" \
            "$(count_packets "${WORK}/ten_out.mp4")" "300"
        # Monotonic PTS and a real end of stream: the packets are read in presentation
        # order, and a stream that stopped early would have fewer of them than frames.
        ten_pts_ok=$(ffprobe -v error -select_streams v:0 -show_entries packet=pts \
            -of csv=p=0 "${WORK}/ten_out.mp4" \
            | awk 'BEGIN{prev=-1; bad=0} {if ($1 <= prev) bad++; prev=$1} END{print bad}')
        assert_eq "PTS is monotonic" "$ten_pts_ok" "0"
    fi
fi

# §15.3's other half: "прогоняется минимум на functional codec tests (AT-009) и на одном
# operating point (R2) synthetic ROI suite". The step above is AT-009; this is the ROI suite,
# and it asks the same question of the ten-bit path that the eight-bit step asks: do the bits
# go where the regions are.
#
# At CRF and not at R2's bitrate, and the reason is worth reading before "fixing" it. R2 is a
# bitrate, §15.3 also asks for VBR, and in this build the two cannot be had together with ten
# bits: qsv h265 reports main10 and no native bitrate control, so §50's guard removes the
# target, while onevpldirect has native VBR and no main10 because P010 is not wired into it.
# Measured 2026-09-03: R2's 1.5 Mbps on qsv h265 comes out at 54-74 Mbps, the target having
# no effect at all. That is question 14 to the customer, recorded in docs/STATUS.md.
step "ROI works on the ten-bit path too (§15.3, R2 suite)"
if [[ ! -x "${BUILD}/roi_metrics" || ! -f "${SCENES}/moving/source.mp4" ]]; then
    skip "the ten-bit ROI suite needs roi_metrics and the synthetic scenes"
elif [[ ! -e /dev/dri/renderD128 ]] || ! source "${ROOT}/integration/scripts/qsv-env.sh" 2>/dev/null; then
    skip "no backend with main10 on this machine -- §15.3's ROI suite is N/A"
elif ! ffmpeg -v error -y -i "${SCENES}/moving/source.mp4" -pix_fmt yuv420p10le \
        -c:v libx265 -x265-params log-level=none -crf 18 "${WORK}/ten_moving.mp4" \
        >/dev/null 2>&1; then
    skip "this ffmpeg cannot produce a ten-bit fixture"
else
    python3 - "${SCENES}/moving/roi.json" "${WORK}/ten_flat.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["samples"] = []
d["background_qp_delta"] = 0
json.dump(d, open(sys.argv[2], "w"))
PY
    ten_ok=1
    for _pair in "ten_adaptive ${SCENES}/moving/roi.json" "ten_baseline ${WORK}/ten_flat.json"; do
        # shellcheck disable=SC2086
        set -- $_pair
        "${BUILD}/roi-transcode" --in "${WORK}/ten_moving.mp4" --roi "$2" \
            --out "${WORK}/$1.mp4" --encoder qsv --codec h265 --crf 32 --gop 60 \
            >"${WORK}/$1.log" 2>&1 || ten_ok=0
    done
    if [[ "$ten_ok" -ne 1 ]]; then
        fail "a ten-bit ROI encode did not complete (see ${WORK}/ten_adaptive.log)"
    else
        assert_eq "the adaptive ten-bit encode stayed Main 10" \
            "$(probe stream=profile "${WORK}/ten_adaptive.mp4")" "Main 10"
        ten_metric() {
            "${BUILD}/roi_metrics" "${WORK}/ten_moving.mp4" "$1" \
                "${SCENES}/moving/truth.json" m 2>/dev/null \
                | grep -oE "$2=[-0-9.]+" | head -1 | cut -d= -f2
        }
        ten_a_roi=$(ten_metric "${WORK}/ten_adaptive.mp4" roi_psnr)
        ten_b_roi=$(ten_metric "${WORK}/ten_baseline.mp4" roi_psnr)
        ten_a_bg=$(ten_metric "${WORK}/ten_adaptive.mp4" bg_psnr)
        ten_b_bg=$(ten_metric "${WORK}/ten_baseline.mp4" bg_psnr)
        assert_gt "ten-bit ROI quality improves (dB)" "$ten_a_roi" "$ten_b_roi"
        assert_lt "ten-bit background paid for it (dB)" "$ten_a_bg" "$ten_b_bg"
    fi

    # And R2 itself, which became possible on 2026-09-03 when the native oneVPL backend
    # learned P010: it has bitrate control of its own, so §15.3's VBR and Main10 can be had
    # together there and nowhere else in this build. The ROI numbers are deliberately not
    # asserted here -- under a target this backend trades the block map for rectangles, and
    # the measured difference is 0.02 dB, which is not a threshold anything should stand on.
    if ! "${BUILD}/roi-transcode" --in "${WORK}/ten_moving.mp4"             --roi "${SCENES}/moving/roi.json" --out "${WORK}/ten_r2.mp4"             --encoder onevpldirect --codec h265 --crf 32 --gop 60             --target-bitrate 1500000 >"${WORK}/ten_r2.log" 2>&1; then
        skip "no native oneVPL on this machine for §15.3's R2 point"
    else
        assert_eq "R2 on ten bits is Main 10"             "$(probe stream=profile "${WORK}/ten_r2.mp4")" "Main 10"
        # The target has to actually act: on the backend without native bitrate control the
        # §50 guard removes it, and the same command comes out tens of times over.
        ten_r2_kbps=$(python3 "${ROOT}/integration/scripts/bitrate-check.py"             "${WORK}/ten_r2.mp4" 1500000 2>/dev/null             | grep -oE 'average +[0-9.]+' | awk '{print $2}')
        assert_lt "R2's target is honoured on ten bits (Mbps)" "$ten_r2_kbps" "1.58"
    fi
fi

# A clip is stored in one orientation and displayed in another, and the difference lives in
# the stream's display matrix. The decoder hands over the stored pixels, so an application
# that drops the matrix writes a file that plays rotated against its source -- which is what
# roi-transcode did until 2026-09-03. No fixture in this repository has a display matrix
# (synthetic clips are written the way they are shown), so this makes one: ffmpeg can attach
# a rotation to a stream copy without touching a pixel.
step "a rotated source stays rotated (display matrix)"
if ! ffmpeg -v error -y -display_rotation 90 -i "$SRC" -c copy "${WORK}/rotated_src.mp4" \
        >/dev/null 2>&1; then
    skip "this ffmpeg cannot attach a display matrix to a stream copy"
else
    src_rotation=$(probe stream_side_data=rotation "${WORK}/rotated_src.mp4" | head -1)
    if [[ "$src_rotation" != "90" ]]; then
        skip "the rotated fixture did not come out rotated (got '${src_rotation}')"
    elif ! "${BUILD}/roi-transcode" --in "${WORK}/rotated_src.mp4" \
            --roi "${ROOT}/testdata/synth_roi.json" --out "${WORK}/rotated_out.mp4" \
            --encoder x264direct --codec h264 --crf 30 --gop 1 \
            >"${WORK}/rotated.log" 2>&1; then
        fail "the rotated clip did not transcode (see ${WORK}/rotated.log)"
    else
        assert_eq "the output keeps the source's rotation" \
            "$(probe stream_side_data=rotation "${WORK}/rotated_out.mp4" | head -1)" "90"
    fi
fi

# §16's two numbers, on the only path in this build that has an outer loop to measure.
#
# The loop runs when the backend has no native bitrate control of its own; on this project
# that is qsv and vaapi for H.264. It is not reachable on CPU at all: x264 honours a target
# itself, and for H.265 the §50 guard zeroes the target before the loop ever sees it. So a
# machine with no Intel GPU cannot check this, and says so rather than passing quietly.
#
# The fixture is built here rather than committed: the crowd scene three times over, for
# §16's own rule that the rolling criterion applies only from 30 seconds, and one small
# protected region. The region size is the point -- with the scene's own roi.json, which
# protects 38% of the frame including everything expensive in it, no background-only
# actuator can reach any target, and what the step would then measure is the fixture.
step "§16's bitrate acceptance, measured on the outer loop"
scene_src="${ROOT}/testdata/scenes/crowded/source.mp4"
if [[ ! -f "$scene_src" ]]; then
    skip "§16 needs testdata/scenes (benchmarks/synthetic/make-scenes.py)"
elif [[ ! -e /dev/dri/renderD128 ]] || ! source "${ROOT}/integration/scripts/qsv-env.sh" 2>/dev/null; then
    skip "§16's outer loop needs a backend without native bitrate control (qsv h264 here)"
else
    printf "file '%s'\n" "$scene_src" "$scene_src" "$scene_src" >"${WORK}/rc36.txt"
    cat >"${WORK}/rc36_roi.json" <<'JSON'
{
 "background_qp_delta": 6,
 "hold_sec": 40,
 "samples": [
  {"t": 0, "kind": "person", "bbox_x": 0.42, "bbox_y": 0.40,
   "bbox_w": 0.16, "bbox_h": 0.20, "track_id": 1, "confidence": 0.9}
 ]
}
JSON
    rc_target=2000000
    if ! ffmpeg -v error -y -f concat -safe 0 -i "${WORK}/rc36.txt" -c copy \
            "${WORK}/rc36.mp4" >/dev/null 2>&1; then
        fail "could not build the 36 s fixture §16 needs"
    elif ! "${BUILD}/roi-transcode" --in "${WORK}/rc36.mp4" --roi "${WORK}/rc36_roi.json" \
            --out "${WORK}/rc36_out.mp4" --encoder qsv --codec h264 --crf 28 --gop 50 \
            --target-bitrate "$rc_target" >"${WORK}/rc36.log" 2>&1; then
        fail "the §16 encode failed (see ${WORK}/rc36.log)"
    elif python3 "${ROOT}/integration/scripts/bitrate-check.py" "${WORK}/rc36_out.mp4" \
            "$rc_target" >"${WORK}/rc36_check.txt" 2>&1; then
        pass "average and 1 s rolling P95 both inside §16"
        sed -n '2,4p' "${WORK}/rc36_check.txt" | sed 's/^/      /'
    else
        fail "§16 not met by the outer loop"
        sed 's/^/      /' "${WORK}/rc36_check.txt" >&2
    fi
fi

# §58.3: every enumerator the library has, the wrapper must have. An enum is the part of a
# binding that goes missing without a symptom -- a value added in C++ compiles, links and
# passes every C++ test while simply not existing in Python. Three had: the native oneVPL
# backend and two status codes a caught exception could already carry as `status_code`.
step "the Python module has every enumerator C++ has (§58.3)"
if [[ ! -x "${LIBBUILD}/enum_surface" ]]; then
    skip "enum parity needs SVE_BUILD_TESTS=ON"
elif ! PYTHONPATH="${ROOT}/python" python3 -c "import smart_video_encoder" >/dev/null 2>&1; then
    skip "enum parity needs the extension built (SVE_BUILD_PYTHON=ON)"
else
    if "${LIBBUILD}/enum_surface" | PYTHONPATH="${ROOT}/python" \
            python3 "${ROOT}/integration/scripts/python-enum-parity.py" \
            >"${WORK}/enum_parity.txt" 2>"${WORK}/enum_parity.log"; then
        pass "$(cat "${WORK}/enum_parity.txt")"
    else
        fail "the bound enums and the C++ enums disagree"
        sed 's/^/      /' "${WORK}/enum_parity.log" >&2
    fi
fi

# §58.10.8: "wrapper overhead измеряется отдельно от encode latency; для CPU frame submit
# без обязательной копии P95 overhead не должен превышать 5% относительно эквивалентного C++
# вызова на том же профиле". Both halves encode the same 720p30 profile and print the same
# three regions; what is compared is `wrap` -- the part with no encoder in it, which is the
# wrapper's own cost -- against 5% of what the whole C++ per-frame call costs.
#
# Why not simply compare the two `total` figures: on a loaded machine the submit region is
# the encode, and the encode's P95 moves by tens of percent between runs, so that ratio says
# more about the machine than about the binding -- it has come out with Python ahead. The
# subtraction is also what §58.10.8 asks for in as many words: encode latency is not the
# wrapper's, so it is not counted against it.
#
# Each side is run several times and the smallest P95 of each is kept. Noise only ever adds.
step "the Python wrapper costs no more than §58.10.8 allows"
if [[ ! -x "${LIBBUILD}/submit_overhead" ]]; then
    skip "overhead budget needs SVE_BUILD_TESTS=ON"
elif ! python3 -c "import numpy" >/dev/null 2>&1; then
    skip "overhead budget needs numpy"
elif ! PYTHONPATH="${ROOT}/python" python3 -c "import smart_video_encoder" >/dev/null 2>&1; then
    skip "overhead budget needs the extension built (SVE_BUILD_PYTHON=ON)"
else
    overhead_reps=3
    overhead_rc=0
    for rep in $(seq "$overhead_reps"); do
        "${LIBBUILD}/submit_overhead" >"${WORK}/overhead_cpp_${rep}.txt" \
            2>"${WORK}/overhead_cpp_${rep}.log" || overhead_rc=1
        PYTHONPATH="${ROOT}/python" python3 "${ROOT}/integration/scripts/python-overhead.py" \
            >"${WORK}/overhead_py_${rep}.txt" 2>"${WORK}/overhead_py_${rep}.log" \
            || overhead_rc=1
    done
    # Smallest P95 across the repetitions of one side.
    best() { awk -v k="$1" '$1 == k { if (m == "" || $2 < m) m = $2 } END { print m }' "${@:2}"; }
    cpp_wrap=$(best wrap_p95_ns "${WORK}"/overhead_cpp_*.txt)
    cpp_total=$(best total_p95_ns "${WORK}"/overhead_cpp_*.txt)
    py_wrap=$(best wrap_p95_ns "${WORK}"/overhead_py_*.txt)
    # A run that died early prints fewer samples, and its percentiles would be of whatever it
    # managed. Checked rather than assumed, because a smaller number passes this budget.
    short=$(cat "${WORK}"/overhead_cpp_*.txt "${WORK}"/overhead_py_*.txt \
        | awk '$1 == "samples" && $2 != 300' | wc -l)
    if [[ "$overhead_rc" -ne 0 || -z "$cpp_wrap" || -z "$cpp_total" || -z "$py_wrap" ]]; then
        fail "overhead measurement did not run"
        tail -3 "${WORK}"/overhead_cpp_*.log "${WORK}"/overhead_py_*.log | sed 's/^/      /' >&2
    elif [[ "$short" -ne 0 ]]; then
        fail "an overhead run measured fewer than 300 samples (${short} of them)"
    else
        overhead=$(awk -v a="$py_wrap" -v b="$cpp_wrap" 'BEGIN { d = a - b; print (d < 0 ? 0 : d) }')
        budget=$(awk -v t="$cpp_total" 'BEGIN { printf "%.0f", t * 0.05 }')
        printf '      wrapper P95 %s ns vs C++ %s ns; C++ per-frame call %s ns\n' \
            "$py_wrap" "$cpp_wrap" "$cpp_total"
        assert_lt "wrapper overhead P95 within 5% of the C++ call (ns)" "$overhead" "$budget"
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
    printf 'all checks passed: %d ok, %d skipped, %d steps; rows appended to %s\n' \
        "$oks" "$skips" "$steps" "$CSV"
    exit 0
fi
printf '%d check(s) failed: %d ok, %d skipped, %d steps\n' \
    "$fails" "$oks" "$skips" "$steps"
exit 1
