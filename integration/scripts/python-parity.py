#!/usr/bin/env python3
"""The Python half of the C++/Python equivalence check -- TZ v4.2 §58.10.1.

Prints the same digest as tests/unit/parity_digest.cpp, for the same configuration and the
same frames. check.sh diffs the two; §58.10 accepts the wrapper only if the two paths give
functionally equivalent packets, PTS, metadata and capability reports, and until 2026-09-02
nothing compared them at all.

The frames are the load-bearing part. Both sides must generate byte-identical input or this
compares the generators rather than the binding, so the pattern is integer arithmetic that
reproduces exactly in NumPy -- the same expression the C++ file spells in a loop.

Run with the package importable:

    PYTHONPATH=python python3 integration/scripts/python-parity.py
"""

import sys

import numpy as np

import smart_video_encoder as sve

# Every one of these has to match parity_digest.cpp. They are the configuration under test,
# not preferences: changing one here without changing it there makes the digests differ for
# a reason that has nothing to do with the binding.
WIDTH, HEIGHT = 160, 120
FPS = 25
FRAMES = 25
CRF = 30
GOP = 10


def planes(n: int) -> list[np.ndarray]:
    """The moving checkerboard of parity_digest.cpp, in integer arithmetic.

    `(col + n*3) // 8 + row // 8` even is lit. Floor division on int arrays, so this is the
    same expression C++ evaluates with `/` on ints -- both operands are non-negative here,
    which is what makes those two the same operation.
    """
    rows, cols = np.mgrid[0:HEIGHT, 0:WIDTH]
    lit = (((cols + n * 3) // 8 + rows // 8) % 2) == 0
    y = np.where(lit, 220, 40).astype(np.uint8)
    u = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    v = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    return [y, u, v]


# FNV-1a, the same constants parity_digest.cpp uses. The digest carries a hash of the input
# so that "the two sides encoded different pixels" and "the two sides encoded the same
# pixels differently" are separable. The first run of this comparison differed and there was
# nothing in the output to say which of the two it was.
FNV_OFFSET_BASIS = 14695981039346656037
FNV_PRIME = 1099511628211
UINT64 = (1 << 64) - 1


def fold(h: int, plane: np.ndarray) -> int:
    for b in plane.tobytes():
        h = ((h ^ b) * FNV_PRIME) & UINT64
    return h


def main() -> int:
    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = WIDTH, HEIGHT
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.input.frame_rate = sve.Rational(FPS, 1)
    cfg.input.time_base = sve.Rational(1, FPS)
    cfg.settings.encoder = sve.Backend.X264_DIRECT
    cfg.settings.backend_selection.preferred = [sve.Backend.X264_DIRECT]
    cfg.settings.codec = sve.Codec.H264
    cfg.settings.crf = CRF
    cfg.settings.gop = GOP
    cfg.settings.metadata.bitstream_sei = False

    try:
        enc = sve.SmartVideoEncoder(cfg)
    except sve.SmartVideoEncoderError as e:
        print(f"python-parity: cannot open x264direct: {e}", file=sys.stderr)
        return 2

    packets = []
    input_hash = FNV_OFFSET_BASIS
    with enc:
        caps = enc.get_capabilities()
        # Booleans as integers, because C++ prints them with %d and "True" is not "1".
        print(f"capability h264={int(caps.h264)} h265={int(caps.h265)} "
              f"roi_rect={int(caps.roi_rectangles)} qp_map={int(caps.qp_map)} "
              f"block={caps.qp_map_block_width}x{caps.qp_map_block_height} "
              f"sei={int(caps.sei)} native_bitrate={int(caps.native_bitrate)} "
              f"main10={int(caps.main10)} p010={int(caps.p010_input)} "
              f"max_regions={caps.max_roi_regions}")

        box = sve.BoundingBox()
        box.kind = "plate"
        box.confidence = 0.9
        box.x, box.y, box.width, box.height = 0.35, 0.40, 0.30, 0.20

        for i in range(FRAMES):
            pl = planes(i)
            for plane in pl:
                input_hash = fold(input_hash, plane)
            frame = sve.frame_from_planes(pl, sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, FPS))
            meta.objects = [box]
            while not enc.submit_frame(frame, meta):
                packets.extend(enc.receive_available())
            packets.extend(enc.receive_available())

        enc.flush()
        while not enc.at_end:
            packets.extend(enc.receive_available())

        stats = enc.get_statistics()

    print(f"input {input_hash:016x}")
    print(f"packets {len(packets)}")
    for i, p in enumerate(packets):
        print(f"packet {i} pts={p.pts} dts={p.dts} key={int(p.key_frame)} "
              f"bytes={len(p.data)}")

    print(f"stats frames={stats.frames_submitted} packets={stats.packets_received} "
          f"bytes={stats.bytes_encoded} converted={stats.frames_converted} "
          f"uploaded={stats.frames_uploaded} regions_dropped={stats.regions_dropped} "
          f"fallbacks={stats.fallbacks_applied} "
          f"clamped={stats.frames_quantiser_clamped} "
          f"guard={int(stats.cpu_h265_target_guard)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
