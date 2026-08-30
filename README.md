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
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-default
cmake --build build -j
ctest --test-dir build
./integration/scripts/fetch-intel-qsv.sh   # once: iHD 25.2.4 + libmfx-gen, no sudo
```

Presets:

| Preset | vcpkg | Builds |
|---|---|---|
| `linux-default` | manifest | library + integration, Debug |
| `linux-release` | manifest | library + integration, Release |
| `linux-cli-only` | manifest | library + CLI, no gRPC |
| `linux-software-only` | manifest | library only, no Intel hardware |
| `linux-dev` | classic (reuses `VCPKG_ROOT`) | library + integration, Debug |

`linux-dev` is the inner loop: it reuses the tree already installed in `VCPKG_ROOT`, so a
reconfigure takes a second. The manifest presets are what a fresh checkout gets, and what
the acceptance criteria ask for -- the first configure with them builds the dependency tree
once.

## Layout

The tree follows TZ v2.0 §52, which describes the **library**. Everything of ours that the
specification does not mention -- the gRPC service, its JSON adapter, the CLI, the ta_vms
scripts -- lives under `integration/` and is built only with `SVE_BUILD_INTEGRATION=ON`.
`check.sh` verifies that separation on every run by configuring the library with the
integration switched off.

## Run

```bash
source integration/scripts/qsv-env.sh
./build/integration/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4
./integration/scripts/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder qsv --crf 28
./build/integration/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder vaapi
./build/integration/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --encoder cpu
./build/integration/roi-transcode --in clip.mp4 --roi testdata/roi.json --out out.mp4 \
  --codec h264
```

`--encoder auto` (default): Quick Sync → VA-API → CPU. `--codec` picks what comes
out; the backend chain is spelled for it, so `--codec h264` gives
`h264_qsv` → `h264_vaapi` → `libx264`.

HEVC saves more and is the default. H.264 exists because a ta_vms archive is
H.264 and one HLS playlist stitches many clips: re-encoding only some of them to
HEVC would leave a single playlist carrying two codecs, which browsers handle
badly.

Quick Sync HEVC on Alder Lake is **EncSliceLP** (VDENC): CQP + `low_power`.
That is also the mode where `mfxExtEncoderROI` `DeltaQP` applies.

Ubuntu 24.04's iHD **24.1** + legacy MediaSDK returns empty HEVC packets
(`Invalid FrameType:0`). `scripts/fetch-intel-qsv.sh` unpacks Intel's
**25.2.4** iHD + `libmfx-gen` into `runtime/` (no sudo). That is a GPU driver
matched to the kernel and the card, which is why it is fetched rather than taken
from vcpkg like everything else.
With `source integration/scripts/qsv-env.sh`, `--encoder qsv` encodes through
`hevc_qsv` → `mfxExtEncoderROI`. Without that env, auto falls back to
`hevc_vaapi` (same iGPU, `VAEncMiscParameterBufferROI`).

ROI JSON uses the same normalized bbox as VMS `track_samples`
(`bbox_x/y/w/h` in 0..1). `t` is seconds from the start of the file.
Detector is not run here — pass boxes in JSON.

`qoffset` is `qp_delta / 51`. QSV turns that into `DeltaQP` on
`mfxExtEncoderROI`.

## Synth eval

```bash
./integration/scripts/make-synth.sh
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
source integration/scripts/qsv-env.sh
APP_ID=roitrc GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT=50061 \
  ROI_ALLOWED_ROOTS="$PWD:/tmp" ./build/integration/roi-transcode-svc
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
./build/integration/roi_grpc_smoke 127.0.0.1:50061 clip.mp4 out.mp4 testdata/roi.json
./build/integration/roi_grpc_smoke 127.0.0.1:50061 clip.mp4 out.mp4 --stream
```

## Using it from another project

One include, one namespace, two ways in. `sve` depends on FFmpeg and the standard
library -- no gRPC, no Dapr, no JSON, nothing from ta_vms -- so a separate project
can `add_subdirectory(roi_transcode)` and link it. Everything that would drag those
in lives under `integration/`, behind an option that is off by default, and
`integration/scripts/check.sh` configures the library alone to keep that true.

```cpp
#include "sve/sve.h"

sve::Settings s;
s.quality.background_qp_delta = 20;
s.quality.kinds["plate"] = sve::KindQuality{-8, 0.01};

// A file.
sve::Report r = sve::transcode_file("in.mp4", "out.mp4", boxes, s);

// Or frames, raw or already compressed, each with a mask or boxes.
sve::SessionParams p;
p.width = 1280; p.height = 720;
p.codec = sve::InputCodec::H264;
p.settings = s;
sve::Session session(p, &handler);   // or p.output_path to mux a file
session.submit(frame, regions);
sve::Report done = session.finish();
```

`PacketHandler` carries no FFmpeg types -- bytes, timestamps and a keyframe flag
-- so writing one needs nothing on the include path but this header.
`tests/public_api_test.cpp` compiles against the public headers alone and fails
if that stops being true.

## Driving the frame mode from a file

`roi-frame-push` exists so the frame mode can be exercised without a gRPC client
in between, and so the two modes can be checked against each other:

```bash
./build/integration/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --push raw
./build/integration/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --push bitstream
./build/integration/roi-frame-push --in clip.mp4 --roi roi.json --out out.mp4 --mask
```

With boxes, `--push raw` and `--push bitstream` encode exactly the same pictures
as `roi-transcode` -- identical framemd5 over the whole stream, identical packet
sizes, identical duration. The containers differ in one field: the frame-push
CLI hands the session the source stream's time base, as an embedding caller
would, while the file path derives one from the frame rate, and FFmpeg 9 carries
that into `mvhd` as a different movie timescale describing the same 12.000 s.
A mask is quantized to the 32-pixel block grid and carries no halo, so it
deliberately differs in the encode itself.

## Seeing the regions on the clip

The encoded file carries no boxes and cannot: ROI encoding changes where the bits
go, and nothing about where they went survives into the bitstream. So the regions
travel beside the video in a `*.boxes.json` sidecar, and `boxes-to-ass.py` turns
that into subtitles any player composites over the picture:

```bash
./integration/scripts/boxes-to-ass.py clip.boxes.json -o clip.ass
mpv clip.roi.mp4 --sub-file=clip.ass
vlc --sub-file=clip.ass clip.roi.mp4          # libass either way
```

Two things this buys over burning the boxes in with `drawbox`. The clip stays
byte-identical to what the encoder produced, so PSNR and SSIM against the source
still mean something -- a burned-in copy is a different picture and every metric
computed on it is wrong. And the same `.ass` fits the source as well as the
re-encoded file, which is how you look at what was asked for and what came out
without generating two overlays.

The sidecar is written by video_a's offline pass (see below); the format is the
one `Box` already uses on the wire -- normalized 0..1 from the top-left -- so the
rectangles land where the encoder was told to spend its bits.

## An end-to-end pass with a real detector

`video_a` (the analytics engine, separate repository) can read a file, detect what
is in it, and drive `EncodeStream` directly over plain gRPC -- no Dapr, no media
router, two processes. `roi-clip.sh` starts both, builds the overlay and opens the
result.

### Quick start (plates)

```bash
./integration/scripts/roi-clip.sh \
  /mnt/wdblue/seenaptec/media_trash/samples/dataset/mycarplate.mp4 \
  -o /mnt/wdblue/seenaptec/media_trash/samples/dataset/output \
  --classes vehicle,license_plate \
  --detect-fps 0 \
  --crf 18 --background-qp 4 \
  --qp license_plate=-12 \
  --encoder cpu --play none

mpv .../output/mycarplate.roi.mp4 --sub-file=.../output/mycarplate.roi.ass
```

Write somewhere the player can open. Snap/Flatpak VLC often cannot read `/tmp/...`
even when the file is valid (`ffprobe` / `mpv` are fine).

### What the knobs mean

| Knob | Role |
|------|------|
| `--crf` | Overall crush. Lower = closer to source, larger file. Default **23**; demos often use 14–18. |
| `--background-qp` | Extra QP on unprotected areas (positive = worse background). Default **6** (mild). 14 was a lab crush. |
| `--qp KIND=DELTA` | Override the v3 ladder for one class (negative = better). Omit to use plate/face/vehicle ladder. |
| `--classes` | What to detect. `license_plate` uses video_a's YOLOv8 plate IR (full frame). |
| `--detect-fps` | Detector rate; **0** = every frame (short plate clips). Default 2. |
| `--target-bitrate` | Outer average ceiling. Does **not** yet match baseline within §16's 2%; leave off for demos. |
| `--encoder` | `cpu` / `vaapi` / `auto`. Prefer `cpu` for quality stands. |
| `-o` / `--out` | Output directory or exact path. |
| `--play` | `roi` \| `source` \| `both` \| `none`. |

Built-in defaults when a flag is omitted: `CRF 23`, `background_qp 6`, `pad 0.02`,
`detect_fps 2`, `codec same`.

```bash
./integration/scripts/roi-clip.sh clip.mp4
./integration/scripts/roi-clip.sh clip.mp4 --crf 18 --background-qp 4 --play both
./integration/scripts/roi-clip.sh clip.mp4 --regions mask --play none
```

Everything lands in `/tmp/roi-clip/<name>/` unless `--out` / `-o` names the result
(`-o` and `--out` are alternatives; the later one wins):

```bash
./integration/scripts/roi-clip.sh /clips/video1.mp4 --out /clips/video1_compressed.mp4
```

Past two or three settings the command line stops being readable, so the same
settings can live in a YAML profile instead — every key is one of the flags, and a
flag still wins over the file:

```bash
cp integration/scripts/roi-clip.example.yaml my-profile.yaml
./integration/scripts/roi-clip.sh -c my-profile.yaml
./integration/scripts/roi-clip.sh -c my-profile.yaml other.mp4 -o /path/out --crf 18
export ROI_CLIP_CONFIG=$PWD/my-profile.yaml   # then just: roi-clip.sh
```

`roi-clip.example.yaml` is the commented list of everything a profile can set
(mild product defaults, not the old CRF36/bg14 crush). An unknown key is refused
by name rather than ignored. `qp` is the one setting that replaces rather than
merges: a `--qp` on the command line stands for the whole block, because a kind
added to the profile's list would otherwise reach the encoder twice.

The encoder service is stopped when the
script exits, and `--play both` opens the source and the result with the same
subtitles -- the coordinates belong to the frame, not to the file, so one overlay
fits both and the comparison is what the encoder was told against what it did.
`--help` lists the encoder and detector knobs it forwards.

By hand, which is what it does:

```bash
# 1. the encoder
APP_ID=roitrc GRPC_ROI_BIND=127.0.0.1 GRPC_ROI_PORT=50061 \
  ROI_ALLOWED_ROOTS=/path/to/clips ./build/integration/roi-transcode-svc

# 2. the pass: decode, detect, re-encode, write the sidecar
ANALYTICS_MODEL_PATH=... ANALYTICS_DEVICE=CPU ANALYTICS_REID_EMBED_INTERVAL_SEC=15 \
  ./analytics-worker --roi-file /path/to/clips/clip.mp4 --roi-grpc 127.0.0.1:50061 \
    --codec h264 --crf 23 --encoder cpu --background-qp 6 \
    --qp license_plate=-12 --pad 0.02

# 3. look at it
./integration/scripts/boxes-to-ass.py /path/to/clips/clip.boxes.json -o /tmp/clip.ass
mpv /path/to/clips/clip.roi.mp4 --sub-file=/tmp/clip.ass
```

Detection is sampled (`--detect-fps`, 2 by default) while decoding is not: every
frame reaches the encoder, and the frames between detections carry the previous
ones forward. `--regions mask` sends the regions as a graded mask instead of
boxes, which is the other half of the contract.

Both paths given to the worker must sit under the service's `ROI_ALLOWED_ROOTS`:
the caller and the service share a filesystem, and the sandbox is checked on the
service's side.

**Do not point this at `--encoder qsv`.** libmfx aborts the service process after
a handful of frames when it encodes from a gRPC worker thread, though the same
frames go through `roi-frame-push --push raw --encoder qsv` on the same machine.
VA-API is the hardware path that works and is the same silicon. Details, and what
the region budget does on VA-API, in [`docs/stream-mode-hw.md`](docs/stream-mode-hw.md).

## Synthetic scenes

```bash
./benchmarks/synthetic/make-scenes.py            # all of them, into testdata/scenes/
./benchmarks/synthetic/make-scenes.py moving
./benchmarks/synthetic/rd_curve.sh moving cpu h264
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
./integration/scripts/check.sh
```

One exit code, no step that needs a human to look at a picture: unit tests, the
service refusing a broken configuration, structural checks on the output, the
ROI assertions, frame/file parity, the gRPC paths and the sandbox, and a
standalone build of `sve`. Each encode also appends a row to
`testdata/eval/runs.csv`, so rate-distortion curves can be built from
accumulated runs without rewriting the script.

Note the assertions differ per backend on purpose. Under CQP (Quick Sync) the QP
offsets are independent, so boosting a zone leaves the background bit-identical
and equality is fair. `libx265` runs CRF, where rate control couples the regions
-- bits spent on the zone are taken from the background -- so there only the
direction is asserted.
