# Source this before roi-transcode / ffmpeg so hevc_qsv hits Intel iHD 25.2
# (Ubuntu 24.04 system iHD 24.1 + MediaSDK returns empty HEVC packets).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INTEL="${ROOT}/third_party/intel-qsv"
LIB="${INTEL}/usr/lib/x86_64-linux-gnu"
if [[ ! -e "${LIB}/dri/iHD_drv_video.so" ]]; then
    echo "missing ${LIB}/dri/iHD_drv_video.so — run scripts/fetch-intel-qsv.sh" >&2
    return 1 2>/dev/null || exit 1
fi
export LD_LIBRARY_PATH="${LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBVA_DRIVERS_PATH="${LIB}/dri"
export LIBVA_DRIVER_NAME=iHD
export ONEVPL_SEARCH_PATH="${LIB}"
