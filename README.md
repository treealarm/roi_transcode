# roi-transcode

Offline HEVC transcoder: keep detector ROIs (plates, faces) sharper and crush
the background. Separate from VMS ingest -- it is a file-in/file-out job, not
part of the live recording path. C++20 / CMake.

Intel encode ROI is **Intel® VPL** `mfxExtEncoderROI` (Quick Sync), not a
custom H.26x codec.

- API: [intel/libvpl](https://github.com/intel/libvpl)
  [`mfxstructures.h`](https://github.com/intel/libvpl/blob/main/api/vpl/mfxstructures.h)
- Runtime: [intel/vpl-gpu-rt](https://github.com/intel/vpl-gpu-rt)
- FFmpeg maps `AVRegionOfInterest` → that API in `hevc_qsv`
  ([commit](https://github.com/FFmpeg/FFmpeg/commit/97141ffeec803c448d81ee4a53cfa2355f79f7ec))

Windows and Linux both work. Linux needs VA-API + `iHD`
([intel/media-driver](https://github.com/intel/media-driver)). Windows uses D3D11.
Needs a live Intel iGPU/Arc. No GPU → `--encoder x265` (CPU, not Intel SDK).

## Build

Headers for Ubuntu FFmpeg 6.1.1 live in `third_party/ffmpeg-dev` (extracted
`-dev` packages). Link against system `libavcodec.so`.

```bash
cmake -S . -B build
cmake --build build -j
./build/roi_map_builder_test
./scripts/fetch-intel-qsv.sh   # once: iHD 25.2.4 + libmfx-gen, no sudo
```

Presets: `linux-default` (Debug + gRPC), `linux-release`, `linux-cli-only`
(no gRPC, no toolchain, no repo protos -- the module builds on its own).

## Run

```bash
source scripts/qsv-env.sh
./build/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4
./scripts/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder qsv --crf 28
./build/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder vaapi
./build/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder x265
```

`--encoder auto` (default): `hevc_qsv` → `hevc_vaapi` → `libx265`.

Quick Sync HEVC on Alder Lake is **EncSliceLP** (VDENC): CQP + `low_power`.
That is also the mode where `mfxExtEncoderROI` `DeltaQP` applies.

Ubuntu 24.04's iHD **24.1** + legacy MediaSDK returns empty HEVC packets
(`Invalid FrameType:0`). `scripts/fetch-intel-qsv.sh` unpacks Intel's
**25.2.4** iHD + `libmfx-gen` into `third_party/intel-qsv` (no sudo).
With `source scripts/qsv-env.sh`, `--encoder qsv` encodes through
`hevc_qsv` → `mfxExtEncoderROI`. Without that env, auto falls back to
`hevc_vaapi` (same iGPU, `VAEncMiscParameterBufferROI`).

ROI JSON uses the same normalized bbox as VMS `track_samples`
(`bbox_x/y/w/h` in 0..1). `t` is seconds from the start of the file.
Detector is not run here — pass boxes in JSON.

`qoffset` is `qp_delta / 51`. QSV turns that into `DeltaQP` on
`mfxExtEncoderROI`.

## Synth eval

```bash
./scripts/make-synth.sh
```

Writes `testdata/synth.mp4` (plate `A123BC 77` on `testsrc2`) and
`testdata/eval/`: QSV encode with/without ROI, plate/bg crops, SSIM.

## gRPC service (+ optional Dapr)

Same job pattern as `ta_vms/media_server` EdgeImport: Start / Get / Cancel /
List. Proto: `../proto/roi_transcode.proto` (repository-wide, with every
other ta_vms service contract).

```bash
cmake --preset linux-default
cmake --build build -j --target roi-transcode-svc
source scripts/qsv-env.sh
APP_ID=roitrc GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT=50061 \
  ROI_ALLOWED_ROOTS="$PWD:/tmp" ./build/roi-transcode-svc
```

| Env | Required | Meaning |
|---|---|---|
| `APP_ID` | yes | service id in logs / events |
| `GRPC_ROI_BIND` | yes | listen address (`127.0.0.1`, or `0.0.0.0` if you need it) |
| `GRPC_ROI_PORT` | yes | listen port |
| `ROI_ALLOWED_ROOTS` | yes | `:`-separated absolute dirs gRPC paths must sit under |
| `DAPR_GRPC_PORT` | no | if set with `PUBSUB_NAME`, publish job changes |
| `PUBSUB_NAME` | no | Dapr pubsub component name |

All four required variables are checked at startup, `ROI_ALLOWED_ROOTS` included:
a misconfigured sandbox fails the process instead of the first request.

Topic: `roi-transcode-job-changed` (JSON job status). Without Dapr env vars,
publish is a no-op.

Request fields: `input_path`, `output_path`, either `roi_json_path` or
repeated `boxes` (+ optional `kinds` / `background_qp_delta` / `hold_sec`),
`encoder`, `crf`, `gop`, `preset`. `background_qp_delta: 0` disables the
full-frame background ROI.

Smoke (against a running svc):

```bash
./build/roi_grpc_smoke [host:port] [in] [out] [roi.json]
```
