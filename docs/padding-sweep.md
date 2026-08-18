# What the halo around a box is worth

`pad_norm` (0.02) and `pad_qp_delta` (`qp_delta / 2`) in `roi_map_builder.cpp`
were never measured. `scripts/sweep-padding.sh` measures them. Run on Quick Sync
on purpose: it is CQP, so QP offsets are independent per block and the
background stays fixed while the zone varies — under x265's CRF the rate control
couples them and every row moves at once. The background SSIM is identical
(0.960478) across all 24 runs, which is the check that the experiment is clean.

Source: `testdata/synth.mp4`, 1280x720, plate at 448,288 384x144, crf 28, GOP 1.

## The halo matters more than the boost

| pad px | qp −4 | qp −8 | qp −12 |
|---|---|---|---|
| 0   | 0.994642 | 0.994890 | 0.995153 |
| 16  | 0.994647 | 0.994891 | 0.995159 |
| 32  | 0.995241 | 0.995459 | 0.995573 |
| 48  | 0.995241 | 0.995459 | 0.995573 |
| 64  | 0.999870 | 0.999921 | 0.999958 |
| 80  | 0.999870 | 0.999921 | 0.999958 |
| 96  | 0.999865 | 0.999922 | 0.999958 |
| 128 | 0.999863 | 0.999918 | 0.999958 |

Plate SSIM. Tripling the boost from −4 to −12 buys 0.0005. Widening the halo
from 0 to 64 px buys 0.005 — ten times more — and then stops: 80, 96 and 128 px
cost bytes and return nothing.

At equal size the comparison is stark:

| configuration | bytes | plate SSIM |
|---|---|---|
| pad 0, qp −12  | 4 994 623 | 0.995153 |
| pad 64, qp −4  | 4 987 144 | **0.999870** |

Same file size to within 0.15%, and roughly 37 times less error in the zone. A
wide halo with a weak boost beats a narrow one with a strong boost.

Two mechanical notes. pad 8 and pad 16 produce byte-identical output, and 32 and
48 likewise: VPL aligns ROI rectangles to the block grid, so intermediate values
round to the same rectangle. And the gain is a cliff, not a ramp — nothing
between 48 and 64, then everything.

## Why the default is not being changed

The saturation point does not scale with the frame. Repeating the sweep on the
same clip scaled to 1920x1080 (`qp −8`):

| pad px | 0 | 32 | 64 | 96 | 128 | 160 | 192 |
|---|---|---|---|---|---|---|---|
| plate SSIM | 0.993212 | 0.999556 | 0.999553 | 0.999582 | 0.999585 | 0.999590 | 0.999582 |

1080p saturates at 32 px where 720p needed 64. That is neither a constant
fraction of the frame (64/1280 = 0.050 vs 32/1920 = 0.017) nor a constant pixel
count, and it moves in the opposite direction to both. Two resolutions are not
enough to tell what the quantity actually is.

So the finding stands and the default does not move yet. What is established:
`pad_norm = 0.02` is below saturation at 720p — 25.6 px, on the flat part of the
curve — and above it at 1080p. Any single fraction is wrong at one end.

What would settle it, in order of cost: a third and fourth resolution on the
synthetic clip; the same sweep with a realistic GOP rather than all-intra, since
these numbers are intra-only and P-frames are known to repair a crushed
background (see the note in `scripts/make-synth.sh`); then the same sweep on
real clips, where box sizes vary and the effect may well be governed by the box
size relative to the coding block grid rather than by the frame at all.

Until then this is a measurement of the mechanism, not a tuned setting — which
is the distinction the whole benchmark plan rests on.
