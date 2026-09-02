#!/usr/bin/env python3
"""The Python half of §58.10.8's wrapper-overhead budget.

§58.10 accepts the wrapper only if, for a CPU frame submit that needs no copy, the P95
overhead stays within 5% of the equivalent C++ call on the same profile. This prints the same
lines tests/unit/submit_overhead.cpp prints; check.sh compares them.

The per-frame path is timed in two regions, which is what §58.10.8 means by measuring the
overhead separately from encode latency:

    frame_from_planes + FrameMetadata   <->   WrapRawFrame          "wrap"
    submit_frame                        <->   SubmitFrame           "submit"

`wrap` has no encoder in it and is the wrapper's own cost; `submit` is dominated by the
encode, which is the same work on both sides; `total` is what an application pays per frame.

The profile is 720p30, the smallest operating point of §39. The wrapper costs a few constant
microseconds per frame -- it copies no pixels (§58.4) -- so the ratio it forms is decided by
what it is divided by, and only a resolution somebody encodes gives an answer worth having.

The arrays are built before the loop, so no NumPy arithmetic of the harness lands in the
measurement, and packets are drained after the clock stops.

Run with the package importable:

    PYTHONPATH=python python3 integration/scripts/python-overhead.py
"""

import sys
import time

import numpy as np

import smart_video_encoder as sve

# Every one of these matches submit_overhead.cpp. A profile that differs on one side makes
# the comparison meaningless in a way no assertion would catch.
WIDTH, HEIGHT = 1280, 720
FPS = 30
CRF = 30
GOP = 100000
SAMPLES = 300
WARMUP = 30


def planes(n: int) -> list[np.ndarray]:
    rows, cols = np.mgrid[0:HEIGHT, 0:WIDTH]
    lit = (((cols + n * 3) // 8 + rows // 8) % 2) == 0
    y = np.where(lit, 220, 40).astype(np.uint8)
    u = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    v = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    return [y, u, v]


def percentile(sorted_samples: list[float], fraction: float) -> float:
    if not sorted_samples:
        return 0.0
    # The same index arithmetic submit_overhead.cpp uses. np.percentile interpolates between
    # neighbours, which is a different number -- small here, but the two sides are being
    # compared to within 5% and a difference in the estimator would be part of the answer.
    return sorted_samples[int(fraction * (len(sorted_samples) - 1))]


def main() -> int:
    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = WIDTH, HEIGHT
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.input.frame_rate = sve.Rational(FPS, 1)
    cfg.input.time_base = sve.Rational(1, FPS)
    cfg.settings.encoder = sve.Backend.X264_DIRECT
    cfg.settings.backend_selection.preferred = [sve.Backend.X264_DIRECT]
    cfg.settings.codec = sve.Codec.H264
    cfg.settings.crf = CRF
    cfg.settings.gop = GOP
    cfg.settings.metadata.bitstream_sei = False
    cfg.log = lambda level, message: None

    try:
        enc = sve.SmartVideoEncoder(cfg)
    except sve.SmartVideoEncoderError as e:
        print(f"python-overhead: cannot open x264direct: {e}", file=sys.stderr)
        return 2

    total = SAMPLES + WARMUP
    prepared = [planes(i) for i in range(total)]
    wrap_ns: list[float] = []
    submit_ns: list[float] = []
    total_ns: list[float] = []

    with enc:
        for i, pl in enumerate(prepared):
            started = time.perf_counter_ns()
            frame = sve.frame_from_planes(pl, sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, FPS))
            wrapped = time.perf_counter_ns()
            accepted = enc.submit_frame(frame, meta)
            finished = time.perf_counter_ns()
            if not accepted:
                # Backpressure (§24). The C++ half drains after every frame too, so this
                # should never happen -- and if it does, the two sides are no longer doing
                # the same thing and the numbers below would not be comparable.
                print(f"python-overhead: backpressure at frame {i}", file=sys.stderr)
                return 2
            if i >= WARMUP:
                wrap_ns.append(float(wrapped - started))
                submit_ns.append(float(finished - wrapped))
                total_ns.append(float(finished - started))
            # Outside the measurement, for the same reason as on the C++ side.
            enc.receive_available()

        enc.flush()
        while not enc.at_end:
            enc.receive_available()

    print(f"samples {len(total_ns)}")
    for name, values in (("wrap", wrap_ns), ("submit", submit_ns), ("total", total_ns)):
        values.sort()
        print(f"{name}_p50_ns {percentile(values, 0.50):.0f}")
        print(f"{name}_p95_ns {percentile(values, 0.95):.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
