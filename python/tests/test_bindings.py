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


# --- the bound surface, and keeping it the same shape as the C++ one --------
#
# Everything below was added on 2026-09-02, after a sweep against §58.3 found that the
# binding had drifted from the C++ structs it mirrors. Three of them mattered: §13's own
# `enabled` switch was unreachable from Python, so the section could not be turned off at
# all; and two of §50's degradation counters were unreadable, so a Python caller could not
# see a quality map that ran off the quantiser scale or a target bitrate refused on CPU
# H.265. Both languages were meant to be the same encoder and were not.
#
# Nothing can make this drift impossible: C++ has no reflection, so no test here can know
# that a field was added to a struct over there. What these do is pin the surface, so that
# a change to it is a deliberate edit to a list somebody reads rather than a silent gap.


def test_the_public_surface_is_the_one_we_intend():
    expected = {
        # §58.3's mandatory bindings.
        "SmartVideoEncoder", "EncoderConfig", "Settings", "TemporalROIConfig",
        "MetadataConfig", "InputVideoFormat", "RateControl",
        # §11's quality model. Bound in _core all along and exported from the package only
        # on 2026-09-02: configuring what a class of object is worth -- the thing this
        # library is for -- meant reaching into a private module until then.
        "Quality", "KindQuality",
        "Codec", "Backend", "BackendSelection", "ObjectPriority", "StatusCode",
        "PixelFormat", "SidecarFormat", "EncoderState", "LogLevel",
        "BoundingBox", "Point", "TrackEvidence", "FrameMetadata", "MediaTimestamp",
        "Rational", "Frame", "EncodedPacket",
        "EncoderCapabilities", "EncoderStatistics",
        # Free functions and module-level values.
        "available_backends", "backend_name", "get_capabilities", "status_code_name",
        "log_level_name", "exception_for_status_code",
        "frame_from_planes", "build_manifest",
        # The exception hierarchy of §58.6.
        "SmartVideoEncoderError", "BackendError", "DeviceUnavailableError",
        "InvalidArgumentError", "UnsupportedError",
    }
    actual = {name for name in dir(sve) if not name.startswith("_")}
    assert actual == expected


@pytest.mark.parametrize("cls,fields", [
    (sve.TemporalROIConfig, {
        "enabled", "max_missing_frames", "max_hold_sec", "confidence_decay",
        "spatial_smoothing", "temporal_smoothing", "max_qp_delta_per_frame",
        "use_track_prediction",
    }),
    (sve.BoundingBox, {
        "t_sec", "kind", "x", "y", "width", "height", "track_id", "confidence",
        "label", "polygon", "track",
    }),
    (sve.TrackEvidence, {"present", "age_sec", "speed", "sample_count"}),
    (sve.RateControl, {
        "target_bitrate", "max_average_overshoot", "max_peak_overshoot",
        "recovery_window_ms",
    }),
])
def test_a_mirrored_struct_exposes_every_field_it_should(cls, fields):
    """These classes mirror a C++ struct field for field, so a missing name is a gap."""
    assert {a for a in dir(cls) if not a.startswith("_")} == fields


def test_temporal_roi_switch_and_patience_round_trip():
    """§13's switch reaches C++ and comes back, rather than being accepted and dropped."""
    cfg = config()
    assert cfg.settings.temporal.enabled is True   # the library's default, not Python's
    cfg.settings.temporal.enabled = False
    cfg.settings.temporal.max_missing_frames = 7
    cfg.settings.temporal.max_hold_sec = 1.5
    assert cfg.settings.temporal.enabled is False
    assert cfg.settings.temporal.max_missing_frames == 7
    assert cfg.settings.temporal.max_hold_sec == pytest.approx(1.5)
    # And the encoder takes it: a value that round-trips through a struct nobody reads
    # would pass the assertions above and mean nothing.
    assert encode(cfg, frames=4)


def test_statistics_expose_the_degradations_section_50_requires():
    """§50 forbids a silent degradation, and these are how two of them become visible."""
    cfg = config()
    with sve.SmartVideoEncoder(cfg) as enc:
        for i in range(4):
            frame = sve.frame_from_planes(planes(16 + i * 5), sve.PixelFormat.YUV420P,
                                          WIDTH, HEIGHT)
            meta = sve.FrameMetadata()
            meta.timestamp = sve.MediaTimestamp(i, sve.Rational(1, 25))
            assert enc.submit_frame(frame, meta)
            enc.receive_available()
        enc.flush()
        while not enc.at_end:
            enc.receive_available()
        stats = enc.get_statistics()

    # Zero on this path, and readable, which is the point: a counter a caller cannot read
    # reports nothing whatever its value.
    assert stats.frames_quantiser_clamped == 0
    assert stats.cpu_h265_target_guard is False
    assert stats.keyframes_requested == 0
    # Under Backend.AUTO this is the only way to find out what actually opened.
    assert "libx264" in stats.backend_name
    assert stats.roi_lag_measured is False   # no tracked boxes were supplied


def test_the_quality_table_is_configurable_without_touching_a_private_module():
    """§11's per-kind table, reachable as sve.KindQuality rather than sve._core.KindQuality."""
    cfg = config()
    plate = sve.KindQuality()
    plate.qp_delta = -8
    plate.pad = 0.05
    cfg.settings.quality.kinds = {"plate": plate}
    cfg.settings.quality.background_qp_delta = 12
    assert cfg.settings.quality.kinds["plate"].qp_delta == -8

    box = sve.BoundingBox()
    box.kind = "plate"
    box.x, box.y, box.width, box.height = 0.4, 0.4, 0.2, 0.15
    with sve.SmartVideoEncoder(cfg) as enc:
        frame = sve.frame_from_planes(planes(), sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
        meta = sve.FrameMetadata()
        meta.timestamp = sve.MediaTimestamp(0, sve.Rational(1, 25))
        meta.objects = [box]
        assert enc.submit_frame(frame, meta)
        enc.flush()
        while not enc.at_end:
            enc.receive_available()
        assert enc.get_statistics().frames_submitted == 1


def test_a_polygon_and_its_track_evidence_survive_the_round_trip():
    """§9's outline and §11's track factors, neither of which Python could reach before."""
    box = sve.BoundingBox()
    box.kind = "plate"
    box.x, box.y, box.width, box.height = 0.3, 0.3, 0.2, 0.1
    outline = []
    for x, y in ((0.30, 0.30), (0.50, 0.30), (0.50, 0.40), (0.30, 0.40)):
        p = sve.Point()
        p.x, p.y = x, y
        outline.append(p)
    box.polygon = outline
    assert len(box.polygon) == 4
    assert box.polygon[1].x == pytest.approx(0.50)

    evidence = sve.TrackEvidence()
    evidence.present = True
    evidence.age_sec = 2.5
    evidence.speed = 0.07
    evidence.sample_count = 12
    box.track = evidence
    assert box.track.present and box.track.sample_count == 12

    # And an encode takes the box with its outline: three points or more make the encoder
    # protect the polygon instead of the axis-aligned box (§9), so this is not decoration.
    cfg = config()
    with sve.SmartVideoEncoder(cfg) as enc:
        frame = sve.frame_from_planes(planes(), sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
        meta = sve.FrameMetadata()
        meta.timestamp = sve.MediaTimestamp(0, sve.Rational(1, 25))
        meta.objects = [box]
        assert enc.submit_frame(frame, meta)
        enc.flush()
        while not enc.at_end:
            enc.receive_available()
        assert enc.get_statistics().frames_submitted == 1


# --- the stubs (§58.10.9) ---------------------------------------------------


def test_the_type_stubs_describe_the_module_that_is_actually_there():
    """py.typed claims this package is typed; this is what keeps the claim true.

    The surface is a compiled extension, so a type checker reading the .so finds Any. The
    stubs are hand-written -- pybind11 emits none and this project carries no stub generator
    on purpose -- which means they can drift in two directions, and both are checked here: a
    binding with no stub leaves a caller with Any while the package promises otherwise, and
    a stub for a binding that no longer exists promises something that will fail at runtime.

    What this does not check is whether the types are *right*. `crf: int` being an int is a
    human claim, the same as any comment. Completeness is the part a machine can hold.
    """
    import ast
    import pathlib

    from smart_video_encoder import _core

    stub_path = pathlib.Path(_core.__file__).with_name("_core.pyi")
    assert stub_path.is_file(), "the package ships py.typed but no stubs"
    tree = ast.parse(stub_path.read_text(encoding="utf-8"))

    declared_top: set[str] = set()
    declared_members: dict[str, set[str]] = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            declared_top.add(node.name)
            names = set()
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and not item.name.startswith("_"):
                    names.add(item.name)
                elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                    if not item.target.id.startswith("_"):
                        names.add(item.target.id)
            declared_members[node.name] = names
        elif isinstance(node, ast.FunctionDef):
            declared_top.add(node.name)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            declared_top.add(node.target.id)

    actual_top = {n for n in dir(_core) if not n.startswith("_")} | {"__version__"}
    assert actual_top - declared_top == set(), "bound but not in the stubs"
    assert declared_top - actual_top == set(), "in the stubs but not bound"

    for name, declared in sorted(declared_members.items()):
        cls = getattr(_core, name)
        if isinstance(cls, type) and issubclass(cls, BaseException):
            # Not comparable this way, and the reason is worth writing down: status_code and
            # status_name are set on the instance by the translator when the exception is
            # raised, so they are not on the class and dir() cannot see them. The stub
            # declares them anyway, which is how a stub says "instance attribute" -- and
            # what makes that declaration true is the test below, which raises one and looks.
            continue
        actual = {a for a in dir(cls) if not a.startswith("_")}
        assert actual == declared, f"{name}: stub and binding disagree"


def test_a_raised_error_carries_the_status_code_the_stubs_promise():
    """§58.6: the C++ answer survives the crossing instead of becoming a message string."""
    cfg = config()
    cfg.input.width = cfg.input.height = 0
    with pytest.raises(sve.InvalidArgumentError) as caught:
        sve.SmartVideoEncoder(cfg)
    err = caught.value
    # Declared on the base class in _core.pyi, set on the instance by the translator. The
    # stub test cannot see instance attributes, so this is what holds that declaration up.
    assert isinstance(err.status_code, int)
    assert err.status_code == sve.StatusCode.INVALID_ARGUMENT.value
    assert err.status_name == sve.status_code_name(sve.StatusCode.INVALID_ARGUMENT)
    assert str(err)   # and the original message is not lost either


# --- §29 through the wrapper -------------------------------------------------


def test_the_library_diagnostics_reach_a_sink_the_application_named():
    """§29: where the library's own diagnostics go is the application's decision.

    Unbound until 2026-09-02, so a Python application had no say at all -- the C++ half of
    integration/scripts/python-parity.py silences its log in one line and the Python half
    could not, which is how this was noticed.
    """
    seen: list[tuple[str, str]] = []
    cfg = config()
    cfg.log = lambda level, message: seen.append((sve.log_level_name(level), message))
    assert encode(cfg, frames=4)

    levels = {level for level, _ in seen}
    text = " | ".join(message for _, message in seen)
    # §29's mandatory events, as they arrive here: which backend opened, what it can do,
    # and the encoder being stopped by the context manager on the way out.
    assert "backend: x264direct" in text
    assert "capabilities:" in text
    assert "encoder stopped" in text
    # And the levels are the library's own, not flattened to one. The capability report is
    # Debug, which the built-in stderr sink drops -- so a sink genuinely sees more, rather
    # than seeing the same lines somewhere else.
    assert {"info", "debug"} <= levels


def test_a_sink_that_raises_does_not_unwind_into_the_encoder():
    """The C++ contract says a sink must not throw; in Python raising is ordinary.

    Without the guard the exception travels out of the log call into code that has a frame
    in flight. With it, the encode finishes and the failure is reported on stderr -- the one
    place that cannot recurse into the sink that just failed.
    """
    calls = []

    def angry(level, message):
        calls.append(message)
        raise RuntimeError("this sink is broken")

    cfg = config()
    cfg.log = angry
    packets = encode(cfg, frames=4)
    assert packets, "the encode completed despite the sink"
    assert calls, "and the sink really was called"


def test_the_log_sink_round_trips_and_can_be_cleared():
    cfg = config()
    assert cfg.log is None
    cfg.log = lambda level, message: None
    assert cfg.log is not None
    cfg.log = None
    assert cfg.log is None


# --- §58.10.4 ownership, and §58.6's mapping in full ------------------------


def test_the_arrays_are_released_when_the_encoder_is_done_with_them():
    """§58.10.4: no reference is leaked by the path that deliberately keeps references.

    `frame_from_planes` does not copy (§58.4), so it must hold the arrays -- and holding
    them is exactly how a wrapper leaks. The count is read before, during and after, so the
    test says both halves: that the reference is taken while it is needed, and that it is
    given back. A test that only checked the second half would pass against a binding that
    never held one and read freed memory.
    """
    import sys

    pl = planes()
    before = [sys.getrefcount(p) for p in pl]

    frame = sve.frame_from_planes(pl, sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
    held = [sys.getrefcount(p) for p in pl]
    assert all(h > b for h, b in zip(held, before)), "the frame does not hold the arrays"

    cfg = config()
    cfg.log = lambda level, message: None
    with sve.SmartVideoEncoder(cfg) as enc:
        meta = sve.FrameMetadata()
        meta.timestamp = sve.MediaTimestamp(0, sve.Rational(1, 25))
        assert enc.submit_frame(frame, meta)
        enc.receive_available()
        enc.flush()
        while not enc.at_end:
            enc.receive_available()

    del frame
    after = [sys.getrefcount(p) for p in pl]
    assert after == before, "the encoder kept a reference to the arrays"


def test_the_frame_holds_the_planes_and_not_the_list_they_arrived_in():
    """§58.4: "скрытое использование dangling pointer запрещено".

    The obvious way to write an encode loop is one list reused per frame -- `pl[0] = luma`
    -- and until 2026-09-02 the Frame held that list rather than the arrays in it, so
    rebinding an element freed a plane the encoder was still reading. Nothing crashed in the
    tests, which is what a use-after-free does until it does not.
    """
    import sys

    pl = planes()
    luma = pl[0]
    frame = sve.frame_from_planes(pl, sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
    pl[0] = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)   # the caller reuses their list

    # Nothing but the local name and the Frame refers to the old luma now. Dropping the
    # Frame must be what lowers the count -- if the list had been the only holder, the count
    # would already be at its floor here and the plane's memory would be free.
    while_held = sys.getrefcount(luma)
    del frame
    assert sys.getrefcount(luma) < while_held, "the Frame did not hold the plane itself"


def test_a_log_sink_is_released_with_the_configuration():
    """§58.10.4 again, for the callbacks: a sink outliving its encoder is a leak too.

    Bound in the same slice as the sink itself (2026-09-02), because a callback held by C++
    is the other shape of the same defect -- and this one holds a Python object from a
    thread the encoder made.
    """
    import sys

    def sink(level, message):
        pass

    before = sys.getrefcount(sink)
    cfg = config()
    cfg.log = sink
    cfg.settings.metadata.sidecar_sink = sink
    assert sys.getrefcount(sink) > before
    encode(cfg, frames=4)
    del cfg
    assert sys.getrefcount(sink) == before, "the configuration kept the sink alive"


def test_encoders_can_be_opened_and_closed_repeatedly():
    """§58.10.4: repeated create/destroy finds double-free and use-after-free, and §58.6
    asks close() to be idempotent -- so both are exercised in the same loop.

    Not a leak assertion in bytes: RSS on a Python process is not a measurement (§43 does
    that properly, in C++, over an hour). What this asserts is that the cycle survives, that
    closing twice is nothing, and that a closed encoder refuses work instead of touching a
    destroyed one.
    """
    cfg = config()
    cfg.log = lambda level, message: None
    for _ in range(25):
        enc = sve.SmartVideoEncoder(cfg)
        frame = sve.frame_from_planes(planes(), sve.PixelFormat.YUV420P, WIDTH, HEIGHT)
        meta = sve.FrameMetadata()
        meta.timestamp = sve.MediaTimestamp(0, sve.Rational(1, 25))
        assert enc.submit_frame(frame, meta)
        enc.close()
        enc.close()   # §58.6: idempotent
        assert enc.closed
        # ValueError, as everywhere else in this suite: using a closed encoder is a caller
        # mistake in Python terms, not a status the library returned.
        with pytest.raises(ValueError):
            enc.submit_frame(frame, meta)


def test_every_status_code_maps_to_an_exception_or_is_documented_as_not_one():
    """§58.10's AT-008 asks for exception mapping for *all* StatusCode, not for the three
    that happen to be reachable from a test.

    The mapping is read from the same function `raise_status` consults, so this cannot
    agree with a table that the raising path does not use. Two codes had no spelling in the
    Python enum at all until 2026-09-02 -- TIMEOUT and ERROR -- which is the shape this
    covers: a code that reaches a caller as `status_code` and matches nothing.
    """
    expected = {
        # Answers, not errors (§58.6): an empty list and a flag say these.
        sve.StatusCode.OK: None,
        sve.StatusCode.AGAIN: None,
        sve.StatusCode.END_OF_STREAM: None,
        # The three decisions a caller branches on differently.
        sve.StatusCode.INVALID_ARGUMENT: sve.InvalidArgumentError,
        sve.StatusCode.UNSUPPORTED: sve.UnsupportedError,
        sve.StatusCode.DEVICE_UNAVAILABLE: sve.DeviceUnavailableError,
        # Everything else, with the code kept on the instance.
        sve.StatusCode.BACKEND_ERROR: sve.BackendError,
        sve.StatusCode.OUT_OF_MEMORY: sve.BackendError,
        sve.StatusCode.TIMEOUT: sve.BackendError,
        sve.StatusCode.ERROR: sve.BackendError,
    }
    bound = set(sve.StatusCode.__members__.values())
    assert bound == set(expected), "a StatusCode with no stated mapping"

    for code, want in expected.items():
        assert sve.exception_for_status_code(code) is want, sve.status_code_name(code)
        if want is not None:
            assert issubclass(want, sve.SmartVideoEncoderError)


def test_the_mapping_is_what_actually_gets_raised():
    """The table above is only worth having if the raising path uses it, so two of its rows
    are checked through a real failure rather than through the function."""
    cfg = config()
    cfg.input.width = cfg.input.height = 0
    with pytest.raises(sve.InvalidArgumentError) as invalid:
        sve.SmartVideoEncoder(cfg)
    assert type(invalid.value) is sve.exception_for_status_code(
        sve.StatusCode.INVALID_ARGUMENT)

    # x265 cannot produce H.264, and says so rather than encoding something else (§8.4).
    # Named explicitly it is the backend refusing; reached through a preference list it is
    # the selection running out of candidates, and §8.4 makes those different answers.
    named = config(backend=sve.Backend.X265_DIRECT)
    named.settings.encoder = sve.Backend.X265_DIRECT
    with pytest.raises(sve.UnsupportedError) as unsupported:
        sve.SmartVideoEncoder(named)
    assert type(unsupported.value) is sve.exception_for_status_code(
        sve.StatusCode.UNSUPPORTED)
    assert unsupported.value.status_code == sve.StatusCode.UNSUPPORTED.value

    with pytest.raises(sve.DeviceUnavailableError) as unavailable:
        sve.SmartVideoEncoder(config(backend=sve.Backend.X265_DIRECT))
    assert type(unavailable.value) is sve.exception_for_status_code(
        sve.StatusCode.DEVICE_UNAVAILABLE)
