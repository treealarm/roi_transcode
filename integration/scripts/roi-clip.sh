#!/usr/bin/env bash
# One clip, from a file on disk to a player with the boxes drawn over it.
#
# Four things have to happen in order and every one of them needs something set up for it: the
# encoder service wants a sandbox and a port, the analytics worker wants its models, the overlay
# wants the sidecar the worker wrote, and the player wants both files at once. Doing that by hand
# is four terminals and a good memory, so this does it instead.
#
#   ./integration/scripts/roi-clip.sh clip.mp4
#   ./integration/scripts/roi-clip.sh clip.mp4 --crf 18 --background-qp 4 --play source
#   ./integration/scripts/roi-clip.sh -c my-profile.yaml
#
# The service is started here and stopped when this exits, so nothing is left listening.
set -euo pipefail

# awk and printf follow the locale, and a comma-decimal one turns 0.02 into "0,02" -- which is not
# a number to the CLI that reads it.
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VIDEO_A="${VIDEO_A_DIR:-$(cd "${ROOT}/../.." && pwd)/video_a}"

SVC="${ROI_SVC_BIN:-${ROOT}/build-int/integration/roi-transcode-svc}"
WORKER="${ANALYTICS_WORKER_BIN:-${VIDEO_A}/build/analytics-worker}"
TO_ASS="${ROOT}/integration/scripts/boxes-to-ass.py"
FROM_YAML="${ROOT}/integration/scripts/roi-clip-config.py"

usage() {
    cat >&2 <<'USAGE'
usage: roi-clip.sh [INPUT.mp4] [options]

Re-encodes INPUT through the ROI encoder using real detections, then opens the result with
the detected objects drawn over it.

  -c, --config FILE    read the settings below from a YAML profile instead of typing them;
                       every key is one of these flags, and a flag still wins over the file.
                       roi-clip.example.yaml is a commented starting point, and
                       ROI_CLIP_CONFIG names one to use when -c is not given.
  -o, --outdir DIR     where the result goes (default: /tmp/roi-clip/<name>)
      --out FILE       name the result exactly; the sidecar and the overlay take its stem
                       and its directory. -o and --out are alternatives: the later wins
      --play WHAT      roi | source | both | none -- which file to open (roi)
      --player NAME    mpv | vlc | ffplay (whichever is installed, mpv first)

  encoding
      --encoder NAME   vaapi | cpu | nvenc | auto   (vaapi when /dev/dri exists, else cpu)
      --codec NAME     h264 | h265 | same           (same as the source)
      --crf N          quantizer                    (encoder default; omit to leave it)
      --gop N          keyframe interval in frames  (same as the source)
      --preset NAME    balanced | fast | slow | ...
      --background-qp N   how much worse everything unprotected gets   (6)
      --qp KIND=DELTA  per-kind override; omit to use the v3 ObjectPriority ladder
      --margin-px N    expand each box by N pixels (service default 8)
      --min-roi-side N expand boxes shorter than N (service default 64)
      --max-qp-delta-per-frame N  limit QP jumps between frames (0 = off)
      --target-bitrate N  outer-loop average ceiling, bits/sec (0 = off; e.g. 500000)
      --bitrate-overshoot F  allowed average overshoot fraction (default 0.1)
      --pad F          soft rim around a box, fraction of the frame        (0.02)
      --max-regions N  cap on regions handed to the encoder
      --regions FORM   boxes | mask | qp_map        (boxes)
      --mask-scale N   mask is 1/N of the frame     (8)
      --qpmap-block N  coding block for qp_map; 0=auto

  detection
      --detect-fps N   inference passes per second of content, 0 = every frame (2)
      --classes LIST   person,face,vehicle,license_plate
      --min-confidence F

  overlay
      --thickness N    box outline width in pixels  (3)
      --keep           leave the service log in the output directory

Defaults (when a flag is omitted): CRF 23, background_qp 6, pad 0.02, detect_fps 2,
codec same. Prefer writing outside /tmp if a Snap/Flatpak player must open the result.

Environment: ROI_CLIP_CONFIG, ANALYTICS_MODEL_PATH (default <video_a>/models), VIDEO_A_DIR,
ROI_SVC_BIN, ANALYTICS_WORKER_BIN, ROI_GRPC_PORT.
USAGE
    exit "${1:-2}"
}

[ $# -ge 1 ] || [ -n "${ROI_CLIP_CONFIG:-}" ] || usage

INPUT=""
OUT_FILE=""
OUTDIR=""
PLAY="roi"
PLAYER=""
THICKNESS=3
KEEP=0
MODELS=""
ENCODER=""
INPUT_FROM=""
OUT_FROM=""
PROFILE_INPUT=""

# Defaults: keep ROI near a usable watchable quality (CRF 23), only a mild background
# penalty (bg_qp 6), and let the v3 ObjectPriority ladder spend the saved bits. Explicit
# --qp KIND=DELTA still wins for those kinds. CRF 36 / bg 14 crushed the frame for the
# early stand and made plates worse than the source — that is not the product default.
declare -A OPT=([--codec]=same [--detect-fps]=2 [--background-qp]=6 [--pad]=0.02 [--crf]=23)
QP_FILE=()
QP_CLI=()

# The profile is read first and the command line second, so plain assignment leaves the flag
# winning. --qp is the one setting that is a set rather than a value: a kind named on the command
# line would otherwise be added to the profile's list instead of replacing it, and both would
# reach the encoder. So each source collects its own, and the more specific one is used whole.
parse_args() {
    local from="$1"; shift
    while [ $# -gt 0 ]; do
        case "$1" in
            # The two are alternatives -- a directory to put the default name in, or the name
            # itself -- so naming one has to unsay the other, or a profile's --out would outlive
            # the -o meant to replace it.
            -o|--outdir)   OUTDIR="$2"; OUT_FILE=""; OUT_FROM="$from"; shift 2 ;;
            --out)         OUT_FILE="$2"; OUTDIR=""; OUT_FROM="$from"; shift 2 ;;
            --play)        PLAY="$2"; shift 2 ;;
            --player)      PLAYER="$2"; shift 2 ;;
            --thickness)   THICKNESS="$2"; shift 2 ;;
            --model-path)  MODELS="$2"; shift 2 ;;
            --keep)        KEEP=1; shift ;;
            --encoder)     ENCODER="$2"; shift 2 ;;
            --qp)          if [ "$from" = file ]; then QP_FILE+=("$2"); else QP_CLI+=("$2"); fi
                           shift 2 ;;
            --target-bitrate|--bitrate-overshoot|\
            --codec|--crf|--gop|--preset|--max-regions|--regions|--mask-scale|--qpmap-block|--background-qp|\
            --pad|--margin-px|--min-roi-side|--max-qp-delta-per-frame|\
            --detect-fps|--classes|--min-confidence)
                           OPT["$1"]="$2"; shift 2 ;;
            -h|--help)     usage 0 ;;
            -*)            echo "unknown option '$1'" >&2; usage ;;
            *)             INPUT="$1"; INPUT_FROM="$from"; shift ;;
        esac
    done
}

CONFIG="${ROI_CLIP_CONFIG:-}"
CLI_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) CONFIG="$2"; shift 2 ;;
        *)           CLI_ARGS+=("$1"); shift ;;
    esac
done

if [ -n "$CONFIG" ]; then
    [ -f "$CONFIG" ] || { echo "no such profile: $CONFIG" >&2; exit 1; }
    [ -x "$FROM_YAML" ] || { echo "no roi-clip-config.py at $FROM_YAML" >&2; exit 1; }
    # Through a file rather than a process substitution: a profile that does not parse has to stop
    # the run, and the exit status of the left side of a `<(...)` is not something bash reports.
    EXPANDED="$(mktemp)"
    trap 'rm -f "$EXPANDED"' EXIT
    "$FROM_YAML" "$CONFIG" > "$EXPANDED"
    FILE_ARGS=()
    mapfile -t -d '' FILE_ARGS < "$EXPANDED"
    rm -f "$EXPANDED"
    trap - EXIT
    parse_args file ${FILE_ARGS[@]+"${FILE_ARGS[@]}"}
    PROFILE_INPUT="$INPUT"
fi
parse_args cli ${CLI_ARGS[@]+"${CLI_ARGS[@]}"}

[ -n "$INPUT" ] || { echo "no input clip given" >&2; usage; }

# A profile that names both the clip and the result describes one conversion. Handed a different
# clip, its `out` would send the result to a name belonging to the first one -- over the top of it,
# if that run has already happened. Say so instead of quietly doing it.
if [ -n "$PROFILE_INPUT" ] && [ "$INPUT_FROM" = cli ] && [ "$OUT_FROM" = file ] &&
   [ "$(readlink -f "$INPUT")" != "$(readlink -f "$PROFILE_INPUT")" ]; then
    echo "${CONFIG} names where $(basename "$PROFILE_INPUT") goes, not $(basename "$INPUT");" \
         "add -o DIR or --out FILE for this one" >&2
    exit 1
fi
INPUT="$(readlink -f "$INPUT")"
[ -f "$INPUT" ] || { echo "no such file: $INPUT" >&2; exit 1; }

NAME="$(basename "${INPUT%.*}")"
[ -n "$OUTDIR" ] || OUTDIR="${ROI_CLIP_OUT:-/tmp/roi-clip}/${NAME}"

WORKER_ARGS=()
for flag in "${!OPT[@]}"; do
    WORKER_ARGS+=("$flag" "${OPT[$flag]}")
done

# No default --qp list: empty kinds lets the service map kind→ObjectPriority ladder.
# A --qp on the profile or the command line is an explicit override and turns the
# ladder off for those kinds (by design -- see Quality::kinds).
QP=()
[ ${#QP_FILE[@]} -gt 0 ] && QP=("${QP_FILE[@]}")
[ ${#QP_CLI[@]} -gt 0 ] && QP=("${QP_CLI[@]}")
for pair in "${QP[@]}"; do
    WORKER_ARGS+=(--qp "$pair")
done

# Quick Sync is left out of the automatic choice on purpose: libmfx aborts the service process
# after a handful of frames when it encodes from a gRPC worker thread. Asking for it explicitly
# still works -- it is a real backend and the bug may be fixed under you -- but you are told.
if [ -z "$ENCODER" ]; then
    ENCODER=$([ -e /dev/dri/renderD128 ] && echo vaapi || echo cpu)
fi
if [ "$ENCODER" = "qsv" ]; then
    echo "note: qsv through the service aborted here on 2026-08-26 and did not " \
         "reproduce on 2026-08-30 (docs/STATUS.md); vaapi is the same silicon" >&2
fi
WORKER_ARGS+=(--encoder "$ENCODER")

[ -x "$SVC" ] || { echo "no roi-transcode-svc at $SVC
build it:  cmake --preset linux-default && cmake --build build -j --target roi-transcode-svc" >&2; exit 1; }
[ -x "$WORKER" ] || { echo "no analytics-worker at $WORKER
build it:  cmake --build ${VIDEO_A}/build --target analytics-worker" >&2; exit 1; }
[ -x "$TO_ASS" ] || { echo "no boxes-to-ass.py at $TO_ASS" >&2; exit 1; }

[ -n "$MODELS" ] || MODELS="${ANALYTICS_MODEL_PATH:-${VIDEO_A}/models}"
[ -d "$MODELS" ] || { echo "no models at $MODELS (set model_path or ANALYTICS_MODEL_PATH)" >&2; exit 1; }

if [ -n "$OUT_FILE" ]; then
    mkdir -p "$(dirname "$OUT_FILE")"
    OUT_MP4="$(readlink -f "$OUT_FILE")"
    OUTDIR="$(dirname "$OUT_MP4")"
    STEM="$(basename "${OUT_MP4%.*}")"
else
    mkdir -p "$OUTDIR"
    OUTDIR="$(readlink -f "$OUTDIR")"
    OUT_MP4="${OUTDIR}/${NAME}.roi.mp4"
    STEM="$NAME"
fi
OUT_JSON="${OUTDIR}/${STEM}.boxes.json"
OUT_ASS="${OUTDIR}/${STEM}.ass"
SVC_LOG="${OUTDIR}/roi-transcode-svc.log"

[ -w "$OUTDIR" ] || { echo "cannot write into ${OUTDIR}" >&2; exit 1; }
# Refuse to overwrite the input rather than destroy it: the encoder muxes into this path, and by
# the time it is halfway through there is nothing left to read frames from.
[ "$OUT_MP4" = "$INPUT" ] && { echo "the output would overwrite the input" >&2; exit 1; }

# A port of our own. The service is started per run, so a fixed one would collide with anything
# already listening -- including a previous run that has not finished shutting down.
port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
PORT="${ROI_GRPC_PORT:-}"
if [ -z "$PORT" ]; then
    PORT=50061
    while ! port_free "$PORT"; do PORT=$((PORT + 1)); done
fi

# The bundled Intel runtime when it has been fetched: the system iHD on Ubuntu 24.04 is older, and
# this is the driver the measurements in docs/ were taken against. Optional -- without it the
# system driver is used, and the CPU encoder needs neither.
if [ -e "${ROOT}/runtime/intel-qsv/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so" ]; then
    # shellcheck source=/dev/null
    source "${ROOT}/integration/scripts/qsv-env.sh"
fi

SVC_PID=""
cleanup() {
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null || true
    [ "$KEEP" -eq 0 ] && rm -f "$SVC_LOG" || true
}
trap cleanup EXIT

[ -n "$CONFIG" ] && echo "==> profile ${CONFIG}"
echo "==> roi-transcode-svc on 127.0.0.1:${PORT}, writing into ${OUTDIR}"
APP_ID=roitrc GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT="$PORT" ROI_ALLOWED_ROOTS="$OUTDIR" \
    "$SVC" > "$SVC_LOG" 2>&1 &
SVC_PID=$!

for _ in $(seq 100); do
    grep -q "listening on" "$SVC_LOG" 2>/dev/null && break
    kill -0 "$SVC_PID" 2>/dev/null || { echo "service died:" >&2; cat "$SVC_LOG" >&2; exit 1; }
    sleep 0.1
done
grep -q "listening on" "$SVC_LOG" || { echo "service did not come up:" >&2; cat "$SVC_LOG" >&2; exit 1; }

echo "==> decoding, detecting and re-encoding (${ENCODER})"
ANALYTICS_MODEL_PATH="$MODELS" \
ANALYTICS_DEVICE="${ANALYTICS_DEVICE:-CPU}" \
ANALYTICS_REID_EMBED_INTERVAL_SEC="${ANALYTICS_REID_EMBED_INTERVAL_SEC:-15}" \
    "$WORKER" --roi-file "$INPUT" --roi-grpc "127.0.0.1:${PORT}" \
        --out "$OUT_MP4" --boxes "$OUT_JSON" "${WORKER_ARGS[@]}" \
    2>&1 | grep -E "roi job|error" || true

[ -s "$OUT_MP4" ] || { echo "the encoder produced nothing; service log:" >&2; cat "$SVC_LOG" >&2; exit 1; }

echo "==> overlay"
"$TO_ASS" "$OUT_JSON" -o "$OUT_ASS" --thickness "$THICKNESS"

in_bytes=$(stat -c%s "$INPUT")
out_bytes=$(stat -c%s "$OUT_MP4")
echo "==> ${in_bytes} -> ${out_bytes} bytes ($(awk "BEGIN{printf \"%.1f\", 100*${out_bytes}/${in_bytes}}")% of the source)"
echo "    ${OUT_MP4}"
echo "    ${OUT_ASS}"

[ "$PLAY" = "none" ] && exit 0

if [ -z "$PLAYER" ]; then
    for candidate in mpv vlc ffplay; do
        command -v "$candidate" >/dev/null && { PLAYER="$candidate"; break; }
    done
fi
[ -n "$PLAYER" ] || { echo "no player found; open ${OUT_MP4} with --sub-file ${OUT_ASS}" >&2; exit 0; }

# The same subtitles fit the source as well as the result -- the coordinates are the frame's, not
# the file's. Playing both is how you see what the encoder was told and what it did with it.
play() {
    echo "==> ${PLAYER}: $(basename "$1")"
    case "$PLAYER" in
        mpv)    mpv --sub-file="$OUT_ASS" --title="$(basename "$1")" "$1" ;;
        vlc)    vlc --sub-file="$OUT_ASS" --play-and-exit "$1" ;;
        ffplay) ffplay -vf "subtitles=${OUT_ASS}" -autoexit "$1" ;;
    esac
}

case "$PLAY" in
    roi)    play "$OUT_MP4" ;;
    source) play "$INPUT" ;;
    both)   play "$INPUT"; play "$OUT_MP4" ;;
    *)      echo "--play wants roi, source, both or none" >&2; exit 2 ;;
esac
