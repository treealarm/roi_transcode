"""The Python binding suite -- TZ v4.2 §58.9.

Run against a build configured with SVE_BUILD_PYTHON=ON, which on this machine also means
without the libavcodec-backed components: a Python extension is a shared object and vcpkg's
FFmpeg is static without -fPIC. That is not a gap in coverage -- §58.9 asks for these tests
on X264Backend and X265Backend, which is exactly what such a build has.
"""

from __future__ import annotations

import threading
import time

import numpy as np
import pytest

import smart_video_encoder as sve

WIDTH, HEIGHT = 320, 240


def planes(fill: int = 16):
    """One YUV420P frame as three arrays."""
    y = np.full((HEIGHT, WIDTH), fill, dtype=np.uint8)
    u = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    v = np.full((HEIGHT // 2, WIDTH // 2), 128, dtype=np.uint8)
    return [y, u, v]


def config(backend=sve.Backend.X264_DIRECT, codec=sve.Codec.H264):
    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = WIDTH, HEIGHT
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.input.frame_rate = sve.Rational(25, 1)
    cfg.input.time_base = sve.Rational(1, 25)
    cfg.settings.codec = codec
    cfg.settings.crf = 30
    cfg.settings.backend_selection.preferred = [backend]
    return cfg


def encode(cfg, frames=12):
    """Submit `frames` frames and drain to end of stream. Returns the packets."""
    packets = []
    with sve.SmartVideoEncoder(cfg) as enc:
        for i in range(frames):
            frame = sve.frame_from_planes(planes(16 + i * 5), sve.PixelFormat.YUV420P,
                                          WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, 25))
            assert enc.submit_frame(frame, meta)
            packets.extend(enc.receive_available())
        enc.flush()
        while not enc.at_end:
            packets.extend(enc.receive_available())
    return packets


def test_import_and_version():
    assert sve.__version__
    assert sve.__version__.count(".") >= 1


def test_build_manifest_describes_this_artefact():
    """§58.7: every field comes from the build, so none can describe a build that was not."""
    manifest = sve.build_manifest
    for field in ("version", "commit", "compiler", "build_type", "cxx_standard",
                  "cxx_abi", "libx264", "libx265", "features", "license"):
        assert manifest[field], f"{field} is empty"
    assert manifest["version"] == sve.__version__
    # Linking libx264/libx265 directly is what puts this under the GPL, and §55 wants that
    # visible before a release rather than discovered by a packager.
    assert "GPL" in manifest["license"]
    # The features are the components this artefact carries, which §5.5 makes a build-time
    # decision -- so a wheel without the direct codecs would be a wheel that cannot encode.
    assert "x264" in manifest["features"]


def test_capability_discovery_without_opening_an_encoder():
    """§58.3: module-level discovery, no encoder required."""
    backends = sve.available_backends()
    assert sve.Backend.X264_DIRECT in backends
    caps = sve.get_capabilities(sve.Backend.X264_DIRECT, sve.Codec.H264)
    assert caps.h264 and caps.roi_rectangles


def test_context_manager_and_idempotent_close():
    enc = sve.SmartVideoEncoder(config())
    assert not enc.closed
    with enc:
        pass
    assert enc.closed
    enc.close()  # §58.6: idempotent
    assert enc.closed


def test_use_after_close_raises_rather_than_touching_a_dead_encoder():
    enc = sve.SmartVideoEncoder(config())
    enc.close()
    with pytest.raises(ValueError):
        enc.get_statistics()


def test_config_and_metadata_round_trip():
    cfg = config()
    assert cfg.input.width == WIDTH
    assert cfg.settings.codec == sve.Codec.H264
    assert cfg.settings.backend_selection.preferred == [sve.Backend.X264_DIRECT]

    meta = sve.FrameMetadata()
    box = sve.BoundingBox()
    box.kind, box.x, box.y, box.width, box.height = "plate", 0.25, 0.25, 0.2, 0.2
    box.confidence, box.track_id = 0.9, 7
    meta.objects = [box]
    assert len(meta.objects) == 1
    assert meta.objects[0].kind == "plate"
    assert meta.objects[0].track_id == 7


def test_wrong_plane_count_is_a_value_error():
    with pytest.raises(ValueError):
        sve.frame_from_planes(planes()[:2], sve.PixelFormat.YUV420P, WIDTH, HEIGHT)


def test_wrong_dtype_is_a_value_error():
    y, u, v = planes()
    with pytest.raises(ValueError):
        sve.frame_from_planes([y.astype(np.uint16), u, v], sve.PixelFormat.YUV420P,
                              WIDTH, HEIGHT)


def test_wrong_shape_is_a_value_error():
    y, u, v = planes()
    with pytest.raises(ValueError):
        sve.frame_from_planes([y[:-1], u, v], sve.PixelFormat.YUV420P, WIDTH, HEIGHT)


def test_column_strided_plane_is_refused():
    """§58.4 allows padded rows, not strides within a row."""
    wide = np.zeros((HEIGHT, WIDTH * 2), dtype=np.uint8)
    y = wide[:, ::2]
    _, u, v = planes()
    with pytest.raises(ValueError):
        sve.frame_from_planes([y, u, v], sve.PixelFormat.YUV420P, WIDTH, HEIGHT)


def test_padded_rows_are_accepted():
    """A row stride larger than a row is normal, and must not be refused."""
    backing = np.zeros((HEIGHT, WIDTH + 32), dtype=np.uint8)
    y = backing[:, :WIDTH]
    _, u, v = planes()
    frame = sve.frame_from_planes([y, u, v], sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
    assert frame.valid()


def test_nv12_and_p010_shapes():
    nv12 = [np.zeros((HEIGHT, WIDTH), dtype=np.uint8),
            np.zeros((HEIGHT // 2, WIDTH), dtype=np.uint8)]
    assert sve.frame_from_planes(nv12, sve.PixelFormat.NV12, WIDTH, HEIGHT).valid()

    p010 = [np.zeros((HEIGHT, WIDTH), dtype=np.uint16),
            np.zeros((HEIGHT // 2, WIDTH), dtype=np.uint16)]
    assert sve.frame_from_planes(p010, sve.PixelFormat.P010, WIDTH, HEIGHT).valid()

    # P010 with eight-bit samples: the shape is right and the memory is half what the
    # encoder would read, which is the mistake worth catching.
    with pytest.raises(ValueError):
        sve.frame_from_planes(nv12, sve.PixelFormat.P010, WIDTH, HEIGHT)


def test_the_owner_survives_the_submit_that_took_it():
    """§58.9's ownership test: drop the arrays after submitting and keep encoding.

    The encoder holds frames past the submit -- lookahead and B-frames are why Flush
    exists -- so without a held reference this is where it would read freed memory.
    """
    packets = []
    with sve.SmartVideoEncoder(config()) as enc:
        for i in range(12):
            arrays = planes(16 + i * 5)
            frame = sve.frame_from_planes(arrays, sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, 25))
            assert enc.submit_frame(frame, meta)
            del arrays, frame
            packets.extend(enc.receive_available())
        enc.flush()
        while not enc.at_end:
            packets.extend(enc.receive_available())
    assert len(packets) == 12


@pytest.mark.parametrize("backend,codec", [
    (sve.Backend.X264_DIRECT, sve.Codec.H264),
    (sve.Backend.X265_DIRECT, sve.Codec.H265),
])
def test_encode_receive_flush_eos(backend, codec):
    if backend not in sve.available_backends():
        pytest.skip(f"{sve.backend_name(backend)} is not in this build")
    packets = encode(config(backend, codec), frames=12)
    assert len(packets) == 12
    assert packets[0].key_frame
    assert all(len(p) > 0 for p in packets)
    assert all(p.codec == codec for p in packets)


def test_every_submitted_timestamp_comes_back_exactly_once():
    """§58.9's PTS mapping, and not trivially true: the codec reorders."""
    packets = encode(config(), frames=12)
    assert sorted(p.pts for p in packets) == list(range(12))


def test_statistics_count_what_went_through():
    with sve.SmartVideoEncoder(config()) as enc:
        for i in range(6):
            frame = sve.frame_from_planes(planes(), sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, 25))
            enc.submit_frame(frame, meta)
        enc.flush()
        while not enc.at_end:
            enc.receive_available()
        stats = enc.get_statistics()
    assert stats.frames_submitted == 6
    assert stats.packets_received == 6
    assert stats.bytes_encoded > 0


def test_an_empty_preference_order_is_an_invalid_argument():
    cfg = config()
    cfg.settings.backend_selection.preferred = []
    with pytest.raises(sve.InvalidArgumentError) as caught:
        sve.SmartVideoEncoder(cfg)
    # §58.6: the code survives, so a caller branches on it rather than on the message.
    assert caught.value.status_code == int(sve.StatusCode.INVALID_ARGUMENT)
    assert caught.value.status_name == "invalid-argument"


def test_encoder_errors_share_one_base():
    cfg = config()
    cfg.settings.backend_selection.preferred = []
    with pytest.raises(sve.SmartVideoEncoderError):
        sve.SmartVideoEncoder(cfg)


def test_asking_x264_for_hevc_is_refused_rather_than_substituted():
    """§8.4: a named backend that cannot do the codec is an error, not a quiet swap."""
    cfg = config(sve.Backend.X264_DIRECT, sve.Codec.H265)
    with pytest.raises(sve.SmartVideoEncoderError):
        sve.SmartVideoEncoder(cfg)


def test_another_thread_runs_while_the_encoder_works():
    """§58.5: the GIL is released around the encode, so Python keeps running.

    Counts how far a plain Python loop gets while a second thread encodes. Without the
    release the counter would barely move; the bound is loose because this is about "not
    blocked", not about speed.
    """
    ticks = 0
    stop = threading.Event()

    def spin():
        nonlocal ticks
        while not stop.is_set():
            ticks += 1
            time.sleep(0.001)

    spinner = threading.Thread(target=spin)
    spinner.start()
    try:
        encode(config(), frames=40)
    finally:
        stop.set()
        spinner.join()

    assert ticks > 10, f"the interpreter only advanced {ticks} times during the encode"
