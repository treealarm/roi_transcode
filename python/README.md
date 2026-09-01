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
