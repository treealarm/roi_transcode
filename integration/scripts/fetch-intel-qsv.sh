#!/usr/bin/env bash
# Unpack Intel GPU client/unified debs (no sudo) for working hevc_qsv on ADL.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${ROOT}/runtime/intel-qsv"
BASE="https://repositories.intel.com/gpu/ubuntu"
mkdir -p /tmp/intel-qsv-debs "$DEST"
cd /tmp/intel-qsv-debs
files=(
  pool/unified/i/intel-media-driver-non-free/intel-media-va-driver-non-free_25.2.4-1146~24.04_amd64.deb
  pool/unified/i/intel-gmmlib/libigdgmm12_22.7.2-1135~24.04_amd64.deb
  pool/unified/i/intel-onevpl/libmfx-gen1_25.2.4-1146~24.04_amd64.deb
  pool/unified/libv/libva/libva-drm2_2.22.0.2-96~u24.04_amd64.deb
  pool/unified/libv/libva/libva-x11-2_2.22.0.2-96~u24.04_amd64.deb
  pool/unified/libv/libva/libva2_2.22.0.2-96~u24.04_amd64.deb
)
for f in "${files[@]}"; do
    curl -fsSL -O "${BASE}/${f}"
done
rm -rf "${DEST}"
mkdir -p "${DEST}"
for deb in *.deb; do
    dpkg-deb -x "$deb" "${DEST}"
done
LIB="${DEST}/usr/lib/x86_64-linux-gnu"
ln -sfn libmfx-gen.so.1.2 "${LIB}/libmfx-gen.so"
echo "extracted to ${DEST}"
echo "source ${ROOT}/integration/scripts/qsv-env.sh"
