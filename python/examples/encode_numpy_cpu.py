#!/usr/bin/env python3
"""Encode CPU frames that live in NumPy arrays -- TZ v4.2 §58.4 and §58.10.9.

The plainest thing the wrapper does: pixels the caller already has, encoded without a copy
on the way in. No ROI, no metadata beyond a timestamp -- see encode_with_roi.py for what
this library is actually for, and capability_report.py for what a machine can do.

The part worth reading twice is the ownership. `frame_from_planes` does not copy: the Frame
holds a reference to the arrays, and the encoder may keep them past the submit that took
them, because codecs buffer for lookahead and B-frames. That is why the arrays are built
inside the loop and simply let go of -- dropping the last Python reference is safe, the
Frame is still holding one. Nothing here has to keep a list alive by hand.
"""

import numpy as np

import smart_video_encoder as sve

WIDTH, HEIGHT, FRAMES = 640, 480, 25
FPS = 25


def moving_edge(n: int) -> list[np.ndarray]:
    """One YUV420P frame as three arrays: a vertical edge that moves across the frame.

    Something has to change between frames or every packet after the first is nearly empty
    and the numbers at the end say nothing.
    """
    xs = np.arange(WIDTH)
    edge = (xs + n * 8) % WIDTH < WIDTH // 2
    y = np.where(edge, 200, 40).astype(np.uint8)
    y = np.broadcast_to(y, (HEIGHT, WIDTH)).copy()
    # Chroma is half resolution in both directions for YUV420P; 128 is neutral.
    u = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    v = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    return [y, u, v]


def main() -> None:
    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = WIDTH, HEIGHT
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.input.frame_rate = sve.Rational(FPS, 1)
    cfg.input.time_base = sve.Rational(1, FPS)
    cfg.settings.codec = sve.Codec.H264
    cfg.settings.crf = 28
    # Named rather than left to Backend.AUTO, so the example does the same thing on every
    # machine. AUTO is the right choice in a real application; see capability_report.py.
    cfg.settings.backend_selection.preferred = [sve.Backend.X264_DIRECT]

    packets = []
    with sve.SmartVideoEncoder(cfg) as enc:
        for i in range(FRAMES):
            frame = sve.frame_from_planes(moving_edge(i), sve.PixelFormat.YUV420P,
                                          WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, FPS))
            # False means the input queue is full: drain and offer the frame again. It is
            # not an error, and this loop is the shape §24 describes.
            while not enc.submit_frame(frame, meta):
                packets.extend(enc.receive_available())
            packets.extend(enc.receive_available())

        enc.flush()
        while not enc.at_end:
            packets.extend(enc.receive_available())

        stats = enc.get_statistics()

    total = sum(len(p.data) for p in packets)
    print(f"backend      : {stats.backend_name}")
    print(f"frames in    : {stats.frames_submitted}")
    print(f"packets out  : {len(packets)}")
    print(f"bytes        : {total}")
    print(f"key frames   : {sum(1 for p in packets if p.key_frame)}")
    # Both zero is what zero-copy means: no pixel format conversion and no upload were
    # needed to get these arrays into the encoder.
    print(f"converted    : {stats.frames_converted}")
    print(f"uploaded     : {stats.frames_uploaded}")
    # §50: a degradation the library applied would show here rather than nowhere.
    print(f"fallbacks    : {stats.fallbacks_applied}")


if __name__ == "__main__":
    main()
