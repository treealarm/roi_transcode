# EncodeStream with raw frames, on hardware

Findings from the first real user of `RoiStreamTranscode` with `INPUT_CODEC_RAW`: video_a's
offline pass (`analytics-worker --roi-file`), which decodes a clip, detects what is in it and
pushes every frame with its regions over plain gRPC. 2026-08-26, `video6.mp4` from
`media_trash/samples/dataset` — 1280x720, 266 frames, 30 fps, 9.8 MB.

Both are about the backend, not about the contract. The stream itself carried 266 frames in and
266 packets out on every backend that survived, and the regions arrived where they were sent —
checked by rendering the sidecar over the result (`scripts/boxes-to-ass.py`), where the boxes sit
on the cars at the frames a detection ran on.

## QSV crashes the service process; the CLIs on the same machine do not

`--encoder qsv` through `roi-transcode-svc` aborts after 5–9 frames:

```
[h264_qsv] Warning during encoding: incompatible video parameters (5)
*** stack smashing detected ***: terminated
```

The stack canary fires inside Intel's driver, not in this repository:

```
#7  __stack_chk_fail ()
#8  iHD_drv_video.so
...
#14 vaEndPicture () from libva.so.2
#15 libmfx-gen.so.1.2.15
#20 start_thread
```

The frame below `start_thread` is libmfx's own worker; it inherits its name from the gRPC thread
that created it, which is why the thread shows as `grpcpp_sync_ser`.

Three things narrow it. It is **not the regions**: `--background-qp 0` with no per-kind offsets,
which sends no `mfxExtEncoderROI` at all, crashes the same way. It is **not the raw-frame path**:
`roi-frame-push --push raw --encoder qsv` encodes all 266 frames of the same clip, through the
same `Session`, with the same settings. And it is **not the hardware**: `--encoder vaapi` reaches
the same iHD driver by the other API and completes.

What is left is the service process itself — libmfx encoding from a thread it did not start,
under a gRPC sync server. Not yet reduced further. Until it is, **the gRPC service must not be
pointed at QSV**; VA-API is the hardware path that works, and it is the same silicon and the same
CQP rate control, which is what the ROI measurements need (see `padding-sweep.md` on why CQP
rather than CRF).

Worth knowing that nothing covered this before: `roi_grpc_smoke --stream` sends compressed access
units, and on this clip the service rejects them (`send packet: Invalid data found`) and falls
back to CPU. So the raw path over gRPC had no test at all.

## VA-API takes eight regions, and the default budget is thirty-two

With four kinds detected on a busy street the pass sends 20–21 rectangles per frame, and libavcodec
says so once per frame:

```
[h264_vaapi] More ROIs set than supported by driver (21 > 8).
```

`Quality::max_regions` defaults to 32 with a comment about mfxExtEncoderROI's 256. Eight is what
this driver actually accepts, and the rest are dropped by libavcodec after this library has already
spent its own budget deciding which to keep — so the choice of *which* eight is made twice, by two
different rules, and the second one knows nothing about kind priority.

The visible consequence: with `background_qp_delta: 8` the background of the frame is **not**
uniformly degraded, because the background is a merged shape that costs many rectangles and most
of them fall off the end. Measured on a sky patch at the top of the frame, PSNR against the source
is identical to six decimals with the background delta on and off. The file does shrink (1 141 972
→ 1 065 474 bytes, 6.7%), so some of it lands; which part is not something the caller can predict.

Sending a mask instead does **not** help, which was worth finding out: a mask is one graded
surface on the wire, but the library turns it into rectangles just the same, and on this clip it
turns into more of them than the boxes did.

| regions | bytes | peak rectangles |
|---|---|---|
| none (`--background-qp 0`) | 1 141 972 | — |
| boxes | 1 065 474 | 21 |
| mask, 1/8 scale | 1 058 591 | 26 |

So the region budget is a property of `rects_from_qp_map`, not of the input form, and nothing a
caller chooses avoids it. The decision that matters — what `max_regions` should be per backend, and
whether kind priority survives libavcodec's own truncation to eight — belongs with the
operating-point work in the plan.

(libavcodec logs the warning once per encoder, not once per frame, so the count in the log says
nothing about how often it happened. The peak above is the number in that one line.)
