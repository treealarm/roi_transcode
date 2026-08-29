# Matched-bitrate acceptance (TZ v3 §15–16)

v3 asks that adaptive and baseline encodes land within **2 %** of the same
average bitrate, then compares ROI quality.

**CPU H.264 (`libx264`):** `--target-bitrate` opens **CBR + filler**
(`EncoderCapabilities::native_bitrate`, `nal-hrd=cbr:filler=1`). Baseline and
adaptive lock to the same HRD rate, which is what meets the 2 % gate. Mux bitrate
typically lands near `target × (1 + peak_overshoot)` (default peak 20%) — the
shared ceiling — rather than exactly on `target`. ROI QP deltas still redistribute
*coded* quality inside that budget; `BitrateBudget` softens background /
non-critical ROI under pressure (and skips §13 QP slew while a target is set so
the ladder can move).

**CPU H.265 (`libx265`):** ROI + SEI path is unstable on the current stack
(`free(): invalid size` / abort in real clips), so H.265 is currently a guarded
mode: it runs without SEI and `--target-bitrate` is downgraded to plain CRF/CQP.
This is an intentional contract for this revision; native H.265 bitrate mode is
considered unsupported until a dedicated fix slice removes the guard.

**HW (QSV / VA-API / NVENC):** still use CQP/CRF + outer `BitrateBudget`;
native CBR there is a later slice.

## Reference profile

| Knob | Value | Notes |
|------|-------|-------|
| GOP | 60 | `Settings::gop` |
| B-frames | 0 | backend default / disable where exposed |
| Rate control (CPU H.264) | CBR + filler when `--target-bitrate` set | `nal-hrd=cbr:filler=1` |
| Rate control (CPU H.265) | guarded CRF/CQP fallback | no native target-bitrate mode |
| Rate control (HW) | CQP/CRF + outer loop | see above |
| AQ | on for CPU (`aq-mode=1`) | required for ROI offsets |

## How to measure

```bash
# From roi_transcode/ after building roi-transcode:
./integration/scripts/matched-bitrate.sh \
  /path/to/video6.mp4 \
  /path/to/boxes.json \
  /tmp/matched-out
# optional 4th arg: target_bps
# optional 5th arg: target tolerance fraction (default 0.20)
```

Uses `roi-transcode` (not `roi-clip`): converts analytics `frames[]` sidecars to
`samples[]` when needed, enables `--target-bitrate` on both baseline (empty ROI)
and adaptive.

Exit 0 only if **both** gates pass:

1. **Parity gate (§16):** `|adaptive - baseline| / baseline ≤ 2%`
2. **Target gate:** both baseline and adaptive are within `target_tol_frac`
   (default 20%) of requested target bitrate.

For CPU H.265 the script should be treated as parity-only for now: because
`--target-bitrate` is intentionally guarded off, target gate interpretation is
informational rather than a pass/fail contract for that codec.

Prefer a target **below** the unconstrained CRF size so CBR is actually stressing
the budget (not padding an already-smaller CRF encode).

## Remaining

1. Dedicated fix slice to remove the CPU H.265 guard (stable ROI + metadata +
   target-bitrate path under tests).
2. QSV / VA-API / NVENC native bitrate that still keeps ROI (or a documented trade-off).
3. `QualityPolicy::BALANCED` / `BITRATE_FIRST` once HW bitrate modes exist.
4. Retire “CRF36 file got smaller” as the primary story — use this script.
