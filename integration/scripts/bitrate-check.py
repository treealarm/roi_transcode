#!/usr/bin/env python3
"""§16's bitrate criteria, measured from an encoded file.

    bitrate-check.py FILE TARGET_BPS [--json]

Two numbers, both normative and both from §16 rather than invented here:

    average bitrate overshoot <= 5%
    P95 of the 1-second rolling bitrate <= 120% target

And one rule that decides whether the second is asked at all: "P95 rolling-статистика
валидна только для последовательностей длительностью >= 30 секунд; для более коротких
rolling-критерий помечается NOT_APPLICABLE". That is why this exists as a separate tool
rather than as three lines inside check.sh -- the repository's only fixture is twelve
seconds long, so the criterion it is meant to check does not apply to it, and a step that
quietly measured it anyway would be reporting a number the specification does not accept.

Rolling windows are stepped one packet at a time rather than one second at a time: a
one-second grid aligned to zero hides a burst that straddles two cells, and hiding bursts
is the one thing a peak criterion must not do.

Packets are read in presentation order. With B-frames the container order is not that, and
a window built from decode order would attribute bits to the wrong second.
"""

import json
import subprocess
import sys


def packets(path):
    """(pts_time, bytes) per video packet, in presentation order."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=pts_time,size", "-of", "csv=p=0", path],
        capture_output=True, text=True, check=True).stdout
    got = []
    for line in out.splitlines():
        parts = line.split(",")
        if len(parts) < 2 or parts[0] in ("N/A", ""):
            continue
        got.append((float(parts[0]), int(parts[1])))
    got.sort()
    return got


def percentile(values, q):
    """The q-th percentile by nearest rank, which needs no interpolation to explain."""
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, min(len(ordered), int(round(q / 100.0 * len(ordered)))))
    return ordered[rank - 1]


def measure(path, target_bps):
    pk = packets(path)
    if len(pk) < 2:
        raise SystemExit(f"{path}: fewer than two video packets")
    frame_sec = pk[1][0] - pk[0][0]
    duration = pk[-1][0] - pk[0][0] + frame_sec
    total_bits = sum(size for _, size in pk) * 8
    average = total_bits / duration

    rolling = []
    first = 0
    window_bytes = 0
    for last, (t, size) in enumerate(pk):
        window_bytes += size
        while pk[first][0] < t - 1.0:
            window_bytes -= pk[first][1]
            first += 1
        if t - pk[0][0] >= 1.0:
            rolling.append(window_bytes * 8)   # a one-second window, so bits == bits/s

    result = {
        "file": path,
        "target_bps": target_bps,
        "duration_sec": round(duration, 3),
        "average_bps": round(average),
        "average_ratio": round(average / target_bps, 4) if target_bps else None,
        "p95_rolling_bps": round(percentile(rolling, 95)),
        "windows": len(rolling),
    }
    result["p95_ratio"] = (round(result["p95_rolling_bps"] / target_bps, 4)
                           if target_bps else None)

    # §16: the average criterion is an overshoot bound, so undershooting does not fail it.
    # It is reported all the same -- an encode at 44% of the bitrate it was asked for met a
    # target the way an empty file meets it, and the number is what says so.
    result["average_overshoot_ok"] = average <= target_bps * 1.05 if target_bps else None
    if duration >= 30.0:
        result["p95_ok"] = result["p95_rolling_bps"] <= target_bps * 1.20
    else:
        result["p95_ok"] = None          # NOT_APPLICABLE, §16
    return result


def main():
    args = [a for a in sys.argv[1:] if a != "--json"]
    if len(args) != 2:
        raise SystemExit(__doc__)
    result = measure(args[0], int(args[1]))
    if "--json" in sys.argv[1:]:
        print(json.dumps(result))
    else:
        p95 = ("NOT_APPLICABLE (<30 s, §16)" if result["p95_ok"] is None
               else f"{result['p95_rolling_bps']/1e6:.2f} Mbps "
                    f"({result['p95_ratio']*100:.0f}% of target, limit 120%) "
                    f"{'ok' if result['p95_ok'] else 'FAIL'}")
        print(f"{result['file']}  {result['duration_sec']:.1f} s")
        print(f"  target        {result['target_bps']/1e6:.2f} Mbps")
        print(f"  average       {result['average_bps']/1e6:.2f} Mbps "
              f"({result['average_ratio']*100:.0f}% of target, overshoot limit 105%) "
              f"{'ok' if result['average_overshoot_ok'] else 'FAIL'}")
        print(f"  P95 rolling   {p95}")
    # Only a criterion that applies can fail the run.
    bad = [k for k in ("average_overshoot_ok", "p95_ok") if result[k] is False]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
