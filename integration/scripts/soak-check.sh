#!/usr/bin/env bash
# Soak -- TZ v4.2 §43.
#
# §43.1 names three profiles: an hour in CI, eight hours nightly on hardware, twenty-four on
# a release candidate. They differ only in how long the driver runs and on which backend, so
# this script is a thin thing around sve-soak that picks a duration and turns its exit code
# into §43.2's five outcomes:
#
#     unexpected crash = 0
#     deadlock = 0
#     frame accounting mismatch = 0
#     metadata mismatch = 0
#     unbounded memory growth = 0
#
# The exit codes are distinct for that reason. 1 means a criterion was not met and the
# driver said which; 3 means its watchdog fired and nothing else could have reported it;
# anything else means the process died on its own, which is the first of the five.
#
#   SOAK_SECONDS=3600 ./integration/scripts/soak-check.sh [build-dir]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-${ROOT}/build}"
SECONDS_TO_RUN="${SOAK_SECONDS:-30}"
ENCODER="${SOAK_ENCODER:-x264direct}"
CODEC="${SOAK_CODEC:-h264}"

SOAK="${BUILD}/sve-soak"
if [[ ! -x "$SOAK" ]]; then
    echo "  skip  sve-soak is not built (SVE_BUILD_TESTS=OFF?)"
    exit 0
fi

echo "  soak  ${SECONDS_TO_RUN}s on ${ENCODER}/${CODEC}"
out=$("$SOAK" --seconds "$SECONDS_TO_RUN" --encoder "$ENCODER" --codec "$CODEC" 2>&1)
rc=$?
printf '%s\n' "$out" | sed 's/^/    /'

case $rc in
    0) echo "soak passed (${SECONDS_TO_RUN}s, §43.2 all zero)"; exit 0 ;;
    1) echo "soak FAILED: an acceptance criterion of §43.2 was not met"; exit 1 ;;
    3) echo "soak FAILED: deadlock -- the watchdog fired"; exit 1 ;;
    *) echo "soak FAILED: unexpected crash (exit ${rc})"; exit 1 ;;
esac
