#!/usr/bin/env python3
"""Encode a few frames with one protected region, and show what it cost.

The point of the library in twenty lines: the same clip encoded twice, once with the
object declared and once without, so the difference is the ROI and nothing else.
"""

import numpy as np

import smart_video_encoder as sve

WIDTH, HEIGHT, FRAMES = 640, 480, 30


def textured_planes(shift):
    """A grid background with a flat plate on it -- the shape the fixtures use.

    Structure rather than noise, and the difference matters. Random noise is incompressible
    either way, so suppressing the background saves nothing and the boost inside the region
    is pure cost: an example built on noise shows ROI making the file larger, which says
    something true about noise and nothing about ROI.
    """
    ys, xs = np.mgrid[0:HEIGHT, 0:WIDTH]
    grid = np.where(((xs + shift) % 16 < 3) | ((ys + shift) % 16 < 3), 235, 60)
    y = grid.astype(np.uint8)
    # The object: a flat bright rectangle where the box will be declared.
    y[int(0.4 * HEIGHT):int(0.6 * HEIGHT), int(0.35 * WIDTH):int(0.65 * WIDTH)] = 200
    u = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    v = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    return [y, u, v]


def run(with_roi: bool) -> int:
    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = WIDTH, HEIGHT
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.input.frame_rate = sve.Rational(25, 1)
    cfg.input.time_base = sve.Rational(1, 25)
    cfg.settings.codec = sve.Codec.H264
    cfg.settings.crf = 32
    cfg.backend_selection.preferred = [sve.Backend.X264_DIRECT]
    if with_roi:
        # Everything outside the object gives up quality for it.
        cfg.settings.quality.background_qp_delta = 20

    total = 0
    with sve.SmartVideoEncoder(cfg) as enc:
        for i in range(FRAMES):
            frame = sve.frame_from_planes(textured_planes(i), sve.PixelFormat.YUV420P,
                                          WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, 25))
            if with_roi:
                box = sve.BoundingBox()
                box.kind, box.confidence = "plate", 0.9
                box.x, box.y, box.width, box.height = 0.35, 0.4, 0.3, 0.2
                meta.objects = [box]
            enc.submit_frame(frame, meta)
            total += sum(len(p) for p in enc.receive_available())
        enc.flush()
        while not enc.at_end:
            total += sum(len(p) for p in enc.receive_available())
    return total


if __name__ == "__main__":
    flat = run(with_roi=False)
    roi = run(with_roi=True)
    print(f"without ROI: {flat:>8} bytes")
    print(f"with ROI:    {roi:>8} bytes  ({100 * roi / flat:.1f}% of flat)")
