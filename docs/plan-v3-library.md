# Library work toward TZ v3 (implementation notes)

Companion to the Cursor plan «План v3 библиотеки». Does **not** replace
[`plan-v2.md`](plan-v2.md): baton for that file is with Danil (see
[`COLLABORATION.md`](COLLABORATION.md)). This file records what landed in code
on the library side so the two tracks stay visible.

**Out of early scope (unchanged):** ta_vms archive/Dapr, NVENC, Rockchip, VMAF.

## Slice A — ObjectPriority ladder (§11.1)

- Public type: [`object_priority.h`](../include/smartvideo/object_priority.h)
  (`Critical/−8` … `Background/0`).
- Continuous v2 importance remains the **internal** way to pick a step
  (`priority_from_importance`); `Quality::use_v3_priority_ladder` (default
  **true**) selects ladder QP vs the old line.
- `Box::desired_qp_delta` overrides the ladder when set.
- Unit: `v3_policy_test`, updated expectations in `importance_test`.

## Slice B — Geometry (§11.2–11.3)

- Defaults: `min_roi_side = 64`, `margin_mode = FixedPixels`, `margin_px = 8`.
- `FractionOfFrame` keeps historical `pad` as halo-only (no body inflate).
- Body margin uses `margin_px` only; `KindQuality.pad` / `Quality.pad` stay the
  soft rim — see [`padding-sweep.md`](padding-sweep.md) (halo **width** matters
  more than QP boost; do not treat `pad=0.02` as v3 truth).

## Slice C — Temporal anti-flicker (§13)

- Defaults match TZ v3: `confidence_decay=0.85`, `spatial_smoothing=0.25`,
  `temporal_smoothing=0.35`, `use_track_prediction=true`, `max_qp_delta_per_frame=2`.
- `QpSlewLimiter` caps QP jumps; `BoxSmoother` EMA on live/file boxes; track
  prediction extrapolates past the last sample within patience.
- `unimplemented_requested()` is always false for temporal — open no longer refuses.

## Slice D — Matched bitrate (§15–16)

- Acceptance helper: [`matched-bitrate.md`](matched-bitrate.md) +
  `integration/scripts/matched-bitrate.sh`.
- **CPU H.264:** `native_bitrate` — CBR + filler (`nal-hrd=cbr:filler=1`); baseline vs
  adaptive within 2% on `video1_12s` (measured). Absolute mux rate sits near
  `target × (1 + peak_overshoot)`.
- Script gate now checks both: adaptive/baseline parity (2%) and distance to
  requested target (default tolerance 20%).
- `BitrateBudget` still fine-tunes object-first; QP slew is skipped while a
  target is active so the ladder can move.
- **CPU H.265 + HW** (QSV/VA-API/NVENC): still CQP + outer loop only.
  CPU H.265 is explicitly guarded: SEI is disabled for stability in ROI runs,
  and `target_bitrate` is downgraded to CRF/CQP with observable telemetry
  (`cpu_h265_target_guard`).

## Slice E — ROI lag (§14)

- Helpers: `src/adaptive/roi_lag.{h,cpp}` — IoU, lag_frames/ms, P95/P99
  acceptance (`IoU≥0.50`, P95≤3 frames/100 ms, P99≤6/200 ms).
- Covered by `v3_policy_test`.
- `transcode_file` and `Session` now expose lag observability in `Report`; both
  CLI paths log one-line runtime stats (`samples`, `min_iou`, lag P95/P99 in
  frames/ms, acceptance).
- Service path exports the same telemetry: `RoiFileTranscode.Job` and stream
  `Done` now carry ROI lag stats and the CPU H.265 target-bitrate guard flag.
- `check.sh` has an optional real-clip lag step and supports a repository-local
  fixture (`testdata/fixtures/real_lag/*`), prepared by
  `integration/scripts/prepare-real-lag-fixture.sh`.

## Proto

v3-first wire (breaking, intentional): `ObjectPriority` + `desired_qp_delta` on `Box`;
`margin_mode` / `margin_px` / `min_roi_side` on `RoiQuality`; `ClassWeighting` (tables
only, no importance→QP line); `TemporalRoiConfig` with implemented fields only.
Vendored into `video_a/proto/roi_transcode.proto`. ta_vms archive callers adjust later.

## Slice F — SEI bitstream metadata (§19)

- Payload encode/decode: `include/smartvideo/metadata.h`, `src/metadata/sei_payload.cpp`
  (UUID + `SVE1` LE frame/objects/CRC32).
- Soft backend `libx264`: `udu_sei=1`, `capabilities.sei=true`; frame side data
  attached from Session / `transcode_file` when `MetadataConfig::bitstream_sei`.
- Soft backend `libx265`: `capabilities.sei=false` (guarded off due to unstable
  ROI+SEI path on current stack).
- Unit: `sei_payload_test`. HW SEI and sidecar file writer still open.

## Current explicit contract (CPU H.265)

- `target_bitrate` on CPU H.265 is a guarded downgrade to CRF/CQP.
- `sei` capability on CPU H.265 is disabled.
- Guard must be visible in logs and in service/report telemetry
  (including explicit SEI-capability warning when bitstream SEI is requested).
- Re-enabling native path requires a dedicated stability slice and regression
  coverage, not a silent behavior change.
- Optional probe for this contract: `integration/scripts/h265-stability-matrix.sh`
  (wired into `check.sh` via `RUN_H265_MATRIX=1`) writes a CSV snapshot of
  guard observability on CPU H.265.
