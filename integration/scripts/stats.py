#!/usr/bin/env python3
"""§41's statistical protocol, applied to numbers this project already measures.

    stats.py summarise            < values         # one sample per line
    stats.py paired A.txt B.txt                    # adaptive against baseline

§41 asks every result for mean, median, stddev, P5, P95 and a 95% confidence interval, and
a paired comparison for baseline against adaptive. Until 2026-09-03 every number recorded
here was a single run, which is not a result in that sense: it carries no statement about
how much of it is the encoder and how much is the machine that ran it.

Two deliberate choices, both from §41 rather than from taste:

* The interval is a **bootstrap** percentile interval, not t-based. §41 names a bootstrap
  for the paired case, and using one estimator for both halves keeps a difference and its
  parts comparable. It also asks nothing of the distribution, which matters here: encode
  times and bitrates are bounded below and have a long upper tail, so normality is exactly
  what one should not assume.
* The paired form resamples **pairs**, not the two sides separately. The runs are matched --
  same clip, same seed, same order -- and breaking the pairing throws away the very thing
  that makes a small number of runs informative.

The seed is fixed and printed, because §41 requires seeds to be pinned in the manifest: a
confidence interval that moves between two readings of the same data is not a measurement.
"""

import random
import statistics
import sys

# Fixed so the interval is reproducible from the same samples, and printed for §41's
# "все seeds фиксируются в манифесте".
SEED = 20260903
# Enough that the percentile bounds are stable to the digits printed here; the cost is
# milliseconds on the sample sizes this project produces.
RESAMPLES = 10000
CONFIDENCE = 0.95


def percentile(sorted_values, fraction):
    """Nearest-rank percentile: no interpolation to explain, and no dependency."""
    if not sorted_values:
        return float("nan")
    index = min(len(sorted_values) - 1, int(fraction * (len(sorted_values) - 1)))
    return sorted_values[index]


def bootstrap_ci(values, statistic, rng):
    """Percentile bootstrap interval for `statistic` over `values`."""
    n = len(values)
    if n < 2:
        return (float("nan"), float("nan"))
    estimates = []
    for _ in range(RESAMPLES):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        estimates.append(statistic(sample))
    estimates.sort()
    tail = (1.0 - CONFIDENCE) / 2.0
    return (percentile(estimates, tail), percentile(estimates, 1.0 - tail))


def summarise(values, label="samples"):
    ordered = sorted(values)
    rng = random.Random(SEED)
    lo, hi = bootstrap_ci(values, statistics.fmean, rng)
    print(f"{label}: n={len(values)} seed={SEED}")
    print(f"  mean   {statistics.fmean(values):.6g}")
    print(f"  median {statistics.median(values):.6g}")
    print(f"  stddev {statistics.stdev(values):.6g}" if len(values) > 1
          else "  stddev n/a (one run)")
    print(f"  P5     {percentile(ordered, 0.05):.6g}")
    print(f"  P95    {percentile(ordered, 0.95):.6g}")
    print(f"  CI95   [{lo:.6g}, {hi:.6g}]")


def paired(adaptive, baseline):
    """§41's paired comparison: the difference, with a bootstrap interval over the pairs."""
    if len(adaptive) != len(baseline):
        print(f"paired needs matched runs: {len(adaptive)} against {len(baseline)}",
              file=sys.stderr)
        return 2
    pairs = list(zip(adaptive, baseline))
    diffs = [a - b for a, b in pairs]
    rng = random.Random(SEED)
    n = len(pairs)
    estimates = []
    for _ in range(RESAMPLES):
        # Pairs are resampled whole, which is what "paired" means: the run is the unit.
        sample = [pairs[rng.randrange(n)] for _ in range(n)]
        estimates.append(statistics.fmean([a - b for a, b in sample]))
    estimates.sort()
    tail = (1.0 - CONFIDENCE) / 2.0
    lo, hi = percentile(estimates, tail), percentile(estimates, 1.0 - tail)
    print(f"paired difference (adaptive - baseline): n={n} seed={SEED}")
    print(f"  mean   {statistics.fmean(diffs):.6g}")
    print(f"  CI95   [{lo:.6g}, {hi:.6g}]")
    # The verdict §41 exists to support: an interval that straddles zero is not a difference,
    # however good the mean looks.
    print(f"  verdict {'separates from zero' if lo > 0 or hi < 0 else 'includes zero'}")
    return 0


def read_numbers(stream):
    return [float(line) for line in stream.read().split() if line.strip()]


def main(argv):
    if len(argv) >= 2 and argv[0] == "paired":
        with open(argv[1]) as a, open(argv[2]) as b:
            return paired(read_numbers(a), read_numbers(b))
    if argv and argv[0] == "summarise":
        values = read_numbers(sys.stdin)
        if not values:
            print("no samples on stdin", file=sys.stderr)
            return 2
        summarise(values)
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
