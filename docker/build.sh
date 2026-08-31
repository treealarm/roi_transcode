#!/usr/bin/env bash
# Builds the two images: the dependency base and the service on top of it.
#
# The base is separate because it takes most of the time and changes almost never --
# it is the vcpkg tree with ffmpeg built for qsv/vaapi. Rebuild it only when
# Dockerfile.deps or vcpkg.json moves.
#
#   ./docker/build.sh            # base if missing, then the service
#   ./docker/build.sh --deps     # force the base to rebuild
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-treealarm/roitrc:latest}"

if [[ "${1:-}" == "--deps" ]] || ! docker image inspect roi-deps >/dev/null 2>&1; then
    echo "==> roi-deps"
    docker build -t roi-deps -f "${ROOT}/docker/intel/Dockerfile.deps" "${ROOT}/docker/intel"
else
    echo "==> roi-deps already present, skipping (--deps to rebuild)"
fi

echo "==> ${IMAGE}"
docker build -t "${IMAGE}" -f "${ROOT}/docker/intel/Dockerfile" "${ROOT}"
