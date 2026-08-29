# SEI metadata -- TZ v3 §19

## What it is

Each encoded frame can carry a **user-data-unregistered SEI** NAL with the objects
that belonged to that frame: `frame_id`, `pts`, boxes, class, priority, optional
desired QP. A player ignores it; a program that knows our UUID can read it back
from the bitstream without a sidecar JSON.

## Format

- FFmpeg side data: `AV_FRAME_DATA_SEI_UNREGISTERED` = 16-byte UUID + payload
- Payload: magic `SVE1`, version 1, little-endian fields, CRC32 (see
  `include/smartvideo/metadata.h` / `src/metadata/sei_payload.cpp`)
- Soft encoders only: libx264 / libx265 with `udu_sei=1` (`capabilities.sei == true`)

## How to turn it on/off

`Settings::metadata.bitstream_sei` defaults to **true**. Set it false to skip.
Hardware backends that cannot insert SEI leave `capabilities.sei == false` and
the library does not attach side data there.

## Not in this slice

- External sidecar file writer (§19.2 "also sidecar")
- In-process `EncodedFrameMetadata` on every `PacketHandler` callback (§19.1) --
  the bitstream form is the deliverable for third-party consumers
- SEI insertion on QSV / VAAPI / NVENC
