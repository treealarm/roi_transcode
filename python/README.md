# smart-video-encoder (Python)

Bindings for the adaptive ROI encoder -- TZ v4.2 §58. The Python layer is thin: encoding,
ROI policy, temporal state, capability mapping, metadata synchronisation and backend
fallback all stay in the C++ core (§58.1).

```python
import numpy as np
import smart_video_encoder as sve

cfg = sve.EncoderConfig()
cfg.input.width, cfg.input.height = 640, 480
cfg.input.pixel_format = sve.PixelFormat.YUV420P
cfg.input.frame_rate = sve.Rational(25, 1)
cfg.input.time_base = sve.Rational(1, 25)
cfg.settings.codec = sve.Codec.H264
cfg.settings.backend_selection.preferred = [sve.Backend.X264_DIRECT]

packets = []
with sve.SmartVideoEncoder(cfg) as enc:
    frame = sve.frame_from_planes([y, u, v], sve.PixelFormat.YUV420P, 640, 480)
    meta = sve.FrameMetadata()
    meta.timestamp = sve.MediaTimestamp(0, sve.Rational(1, 25))
    enc.submit_frame(frame, meta)
    packets += enc.receive_available()
    enc.flush()
    while not enc.at_end:
        packets += enc.receive_available()
```

Three things worth knowing before the first surprise:

* `submit_frame` returns `False` rather than raising when the queue is full. That is §24's
  backpressure and an ordinary answer -- drain and submit again.
* `receive_available` returning `[]` means "nothing ready yet", not an error. End of stream
  is the `at_end` property. §58.6 asks for both to be documented rather than raised.
* the arrays you pass are held for as long as the encoder needs them, which is past the
  submit that took them: codecs buffer. You may drop your own references immediately.

## API reference

Everything below is importable from `smart_video_encoder`. The package ships `py.typed` and
stubs beside the extension, so a type checker sees these signatures rather than `Any`;
`tests/test_bindings.py` compares the stubs against the built module in both directions, so
they cannot describe a surface that is not there.

### The encoder

`SmartVideoEncoder(config: EncoderConfig)` — a context manager, and `close()` is idempotent
(§58.6). After closing, every operation raises rather than touching a dead encoder.

| Member | Returns | Notes |
|---|---|---|
| `submit_frame(frame, metadata)` | `bool` | `False` is backpressure (§24), not an error |
| `receive_available()` | `list[EncodedPacket]` | `[]` means nothing ready yet |
| `flush()` | `None` | no more frames; drain until `at_end` |
| `at_end` | `bool` | end of stream reached |
| `closed` | `bool` | |
| `close()` | `None` | idempotent |
| `get_capabilities()` | `EncoderCapabilities` | what the backend that opened can do |
| `get_statistics()` | `EncoderStatistics` | counts, degradations, which backend ran |

The GIL is released across `submit_frame`, `receive_available` and `flush` (§58.5), so
another Python thread runs while the encoder works. One encoder is driven by one thread at a
time, exactly as the C++ API states; separate encoders on separate threads are supported and
are the way to encode several streams at once.

### Configuration

`EncoderConfig` has two halves -- `input` (`InputVideoFormat`) and `settings` (`Settings`) --
plus `log`.

`log: Callable[[LogLevel, str], None] | None` is §29's sink: where the library's own
diagnostics go. Left unset they go to stderr, one line each, which is what a command-line
tool wants. A sink sees more than stderr does, because the built-in one drops `DEBUG` and
`TRACE` and has no way to be turned down -- the capability report at open is `DEBUG`. It may
be called from a thread the encoder made, and a sink that raises does not unwind into the
encoder: the exception is caught and reported on stderr instead.

The events §29 requires are backend selected, capability report, fallback, unsupported
feature, encoder reset/reconfigure, frame drop and metadata failure.

* `InputVideoFormat` — `width`, `height`, `pixel_format`, `frame_rate`, `time_base`.
* `Settings` — `encoder`, `backend_selection`, `codec`, `crf`, `gop`, `quality`, `temporal`,
  `rate_control`, `metadata`.
* `BackendSelection` — `preferred: list[Backend]`, `allow_unlisted: bool`. Under
  `Backend.AUTO` the order is walked; a named backend is tried alone, so asking for one and
  silently getting another cannot happen (§8.4).
* `Quality` (§11) — `kinds: dict[str, KindQuality]`, `background_qp_delta`, `pad`,
  `margin_px`, `min_roi_side`, `max_regions`, `max_protected_fraction`, `hold_sec`,
  `mask_levels`, `mask_qp_delta_at_255`. `KindQuality` is `qp_delta` and `pad`: what one
  class of object is worth.
* `TemporalROIConfig` (§13) — `enabled`, `max_missing_frames`, `max_hold_sec`,
  `confidence_decay`, `spatial_smoothing`, `temporal_smoothing`, `max_qp_delta_per_frame`,
  `use_track_prediction`. `enabled=False` means the boxes the producer supplied at the times
  it supplied them, and nothing else.
* `RateControl` (§15) — `target_bitrate`, `max_average_overshoot`, `max_peak_overshoot`,
  `recovery_window_ms`.
* `MetadataConfig` (§19) — `bitstream_sei`, `max_objects`, `sidecar`, `sidecar_sink`. The
  sink is called once per emitted packet, in emission order, with one complete record and no
  trailing newline.

### Objects and frames

* `frame_from_planes(planes, pixel_format, width, height) -> Frame` — wraps arrays without
  copying (§58.4). Rows may be padded; anything whose dtype, shape or strides do not fit
  raises `ValueError` or `TypeError` rather than being read wrongly later. The `Frame` holds
  a reference to the arrays, so you may drop yours.
* `FrameMetadata` — `timestamp`, `force_key_frame`, `objects: list[BoundingBox]`.
* `BoundingBox` (§9) — `kind`, `label`, `x`, `y`, `width`, `height` (normalized 0..1),
  `confidence`, `track_id`, `t_sec`, `polygon: list[Point]`, `track: TrackEvidence`. Three
  or more polygon points and the encoder protects the outline rather than the box.
* `TrackEvidence` (§11) — `present`, `age_sec`, `speed`, `sample_count`.

### What comes back

* `EncodedPacket` — `data: bytes`, `pts`, `dts`, `duration`, `time_base`, `key_frame`,
  `codec`.
* `EncoderStatistics` — counts (`frames_submitted`, `packets_received`, `bytes_encoded`),
  zero-copy evidence (`frames_converted`, `frames_uploaded`: both zero is what it means),
  which backend ran (`encoder_used`, `backend_name`, `backends_declined`), and the
  degradations §50 forbids applying silently: `regions_dropped`, `fallbacks_applied`,
  `frames_quantiser_clamped`, `cpu_h265_target_guard`. §14's ROI lag arrives in the
  `roi_lag_*` fields when the boxes carried track ids to measure it from.
* `EncoderCapabilities` — `h264`, `h265`, `roi_rectangles`, `qp_map` with
  `qp_map_block_width`/`_height`, `ctu_qp`, `emphasis_map`, `runtime_roi_update`, `sei`,
  `native_bitrate`, `main10`, `p010_input`, `max_roi_regions` (0 means no limit it can name).

### Capability discovery, without opening an encoder

`available_backends() -> list[Backend]`, `get_capabilities(backend, codec) ->
EncoderCapabilities`, `backend_name(backend) -> str`, `status_code_name(code) -> str`,
`log_level_name(level) -> str`.
§58.3 asks for this at module level because choosing a backend and finding out afterwards
that it cannot do what you need is a failed encode, while asking first is a branch.

`__version__` and `build_manifest` say what this artefact is: version, commit, compiler,
the libx264/libx265 versions inside it, and its licence terms.

### Exceptions (§58.6)

`SmartVideoEncoderError` is the base; `InvalidArgumentError`, `UnsupportedError`,
`DeviceUnavailableError` and `BackendError` derive from it. Each carries the originating
status as `status_code` (the enumerator's integer value) and `status_name` (its spelling),
so the C++ answer survives the crossing rather than being flattened into a message string.
`Again` and `EndOfStream` are **not** exceptions -- they are the `False` from
`submit_frame` and the `at_end` property, because neither is a failure.

## Examples

In `examples/`, and all three run against a build of this package:

| File | Shows |
|---|---|
| `encode_numpy_cpu.py` | the plainest path: NumPy frames in, packets out, no copy |
| `encode_with_roi.py` | the same clip with and without a declared object, so the difference is the ROI |
| `capability_report.py` | what this build can do here, without opening an encoder |

## What the wrapper costs (§58.10.8)

Frames are not copied (§58.4), so what the wrapper adds per frame is a constant: building
the `Frame` and the `FrameMetadata` and crossing into C++. Measured on 720p30 with x264
direct, that is a few tens of microseconds against a per-frame C++ call of a few
milliseconds -- under 1%, where §58.10.8 allows 5%.

The two halves of the measurement are `tests/unit/submit_overhead.cpp` and
`integration/scripts/python-overhead.py`; `check.sh` runs both and compares them. The
resolution decides the answer: the same wrapper on a 160x120 frame is a 9% tax, because the
constant has not changed and the encode it is compared against has almost vanished.

## Building

```bash
cmake -S . -B build-python -DSVE_BUILD_PYTHON=ON \
      -DSVE_BUILD_BACKEND_FFMPEG=OFF -DSVE_BUILD_EXAMPLES=OFF
cmake --build build-python
cd python && python3 -m pytest tests/
```

The libavcodec-backed backends are off because a Python extension is a shared object and
this machine's FFmpeg is a static library built without `-fPIC`. What remains is §5.5's
dependency graph -- the core and the direct libx264/libx265 components -- which is what
§58.9 asks the Python tests to run against anyway.
