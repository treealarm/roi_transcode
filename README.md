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

The tree follows TZ v2.0 §52, which describes the **library**. Everything the
specification does not mention -- the CLI, its JSON adapter and the scripts -- lives under
`integration/` and is built only with `SVE_BUILD_INTEGRATION=ON`. `check.sh` verifies that
separation on every run by configuring the library with the integration switched off.

The gRPC service that used to sit there moved to the ta_vms repository on 2026-08-31: a
job model, Dapr and a path sandbox are not in this specification, and keeping them here
meant building grpc and protobuf beside a library that needs neither.

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

The sidecar format is the one `Box` uses on the wire -- normalized 0..1 from the
top-left -- so the rectangles land where the encoder was told to spend its bits.
Whatever produced the boxes can write one; `roi.json` is already in that shape.

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

## Docker

```bash
docker build -t roi-deps -f docker/intel/Dockerfile.deps docker/intel
```

`roi-deps` is a build environment, not a product: the C++ toolchain and the vcpkg tree with
FFmpeg built for qsv/vaapi. It takes most of the build time and changes almost never, which
is why it is an image of its own. ta_vms builds its service on top of it.

## Checks

```bash
./integration/scripts/check.sh
```

One exit code, no step that needs a human to look at a picture: unit tests, structural
checks on the output, the ROI assertions, frame/file parity, and a standalone build of
`sve`. Nothing in it goes through a network: the library is driven directly, and the frame
API through `roi-frame-push` rather than a client. Each encode also appends a row to
`testdata/eval/runs.csv`, so rate-distortion curves can be built from
accumulated runs without rewriting the script.

Note the assertions differ per backend on purpose. Under CQP (Quick Sync) the QP
offsets are independent, so boosting a zone leaves the background bit-identical
and equality is fair. `libx265` runs CRF, where rate control couples the regions
-- bits spent on the zone are taken from the background -- so there only the
direction is asserted.
