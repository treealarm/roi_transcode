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

Everything comes from vcpkg: FFmpeg (with `qsv` and `vaapi`), nlohmann-json,
protobuf and gRPC. Nothing is vendored.

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
**25.2.4** iHD + `libmfx-gen` into `runtime/` (no sudo). That is a GPU driver
matched to the kernel and the card, which is why it is fetched rather than taken
from vcpkg like everything else.
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

Two entry points, one `Settings` shared by both. Proto:
`../proto/roi_transcode.proto` (repository-wide, with every other ta_vms
service contract).

- **File job** — `StartFileJob` / `GetJob` / `CancelJob` / `ListJobs`, the same
  pattern as `ta_vms/media_server` EdgeImport: start returns immediately, poll
  for the result, cancel is best effort.
- **Frame stream** — `EncodeStream`, a bidirectional stream for a caller with no
  file. First message is `open`, then frames, each carrying a mask or boxes.
  With `output_path` set the packets are muxed there; otherwise they come back
  over the stream.

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

Smoke (against a running svc) covers both:

```bash
./build/roi_grpc_smoke 127.0.0.1:50061 clip.mp4 out.mp4 testdata/roi.json
./build/roi_grpc_smoke 127.0.0.1:50061 clip.mp4 out.mp4 --stream
```

## Using it from another project

One include, one namespace, two ways in. `roi_core` depends on FFmpeg and the
standard library -- no gRPC, no Dapr, no JSON, nothing from ta_vms -- so a
separate project can `add_subdirectory(roi_transcode/core)` and link it -- `core/`
is its own CMake project, and `scripts/check.sh` configures it alone to keep that true.

```cpp
#include "roi/roi.h"

roi::Settings s;
s.quality.background_qp_delta = 20;
s.quality.kinds["plate"] = roi::KindQuality{-8, 0.01};

// A file.
roi::Report r = roi::transcode_file("in.mp4", "out.mp4", boxes, s);

// Or frames, raw or already compressed, each with a mask or boxes.
roi::SessionParams p;
p.width = 1280; p.height = 720;
p.codec = roi::InputCodec::H264;
p.settings = s;
roi::Session session(p, &handler);   // or p.output_path to mux a file
session.submit(frame, regions);
roi::Report done = session.finish();
```

`PacketHandler` carries no FFmpeg types -- bytes, timestamps and a keyframe flag
-- so writing one needs nothing on the include path but this header.
`tests/public_api_test.cpp` compiles against the public headers alone and fails
if that stops being true.

## Driving the frame mode from a file

`roi-frame-push` exists so the frame mode can be exercised without a gRPC client
in between, and so the two modes can be checked against each other:

```bash
./build/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --push raw
./build/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --push bitstream
./build/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --mask
```

With boxes, `--push raw` and `--push bitstream` encode exactly the same pictures
as `roi-transcode` -- identical framemd5 over the whole stream, identical packet
sizes, identical duration. The containers differ in one field: the frame-push
CLI hands the session the source stream's time base, as an embedding caller
would, while the file path derives one from the frame rate, and FFmpeg 9 carries
that into `mvhd` as a different movie timescale describing the same 12.000 s.
A mask is quantized to the 32-pixel block grid and carries no halo, so it
deliberately differs in the encode itself.

## Synthetic scenes

```bash
./scripts/make-scenes.py            # all of them, into testdata/scenes/
./scripts/make-scenes.py moving
```

Five clips, each with a `truth.json` saying where the object is on every frame,
and a `roi.json` the CLI takes directly. The backgrounds differ in what they
stress: `flat` has no texture to hide behind, `edges` is where ringing shows up
first, `zoneplate` sweeps spatial frequency, `texture` is high-entropy, and
`moving` is the only one where the box does not sit still.

The ground truth is the point. A mask built from the same guess the encoder was
given proves nothing; one built from `truth.json` is independent of it. `check.sh`
verifies the truth actually describes the video before trusting it -- a clip and
a JSON that disagreed would make every mask derived from them a lie, and nothing
downstream would notice.

## Checks

```bash
./scripts/check.sh
```

One exit code, no step that needs a human to look at a picture: unit tests, the
service refusing a broken configuration, structural checks on the output, the
ROI assertions, frame/file parity, the gRPC paths and the sandbox, and a
standalone build of `roi_core`. Each encode also appends a row to
`testdata/eval/runs.csv`, so rate-distortion curves can be built from
accumulated runs without rewriting the script.

Note the assertions differ per backend on purpose. Under CQP (Quick Sync) the QP
offsets are independent, so boosting a zone leaves the background bit-identical
and equality is fair. `libx265` runs CRF, where rate control couples the regions
-- bits spent on the zone are taken from the background -- so there only the
direction is asserted.
