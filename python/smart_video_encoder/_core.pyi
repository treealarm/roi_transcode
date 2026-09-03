# Type stubs for the extension module -- TZ v4.2 §58.10.9, which asks for type hints
# alongside the API reference and the examples.
#
# The package ships py.typed, which is a claim that it is typed. Until 2026-09-02 that claim
# had nothing behind it: the surface is a compiled extension, and a type checker looking at a
# .so finds Any everywhere. Promising types and delivering none is worse than promising
# nothing, so this file is what makes the marker true.
#
# **Hand-written, and checked for completeness rather than trusted.** pybind11 emits no
# stubs and this project does not carry a stub generator (its dependency profile is a
# deliberate property -- see docs/STATUS.md, decision Р1). Every type here was read off the
# running module rather than guessed, and tests/test_bindings.py compares this file's
# declared names against the module's actual ones in both directions, so a binding added
# without a stub, or a stub for a binding that no longer exists, fails the suite.
#
# The types are still a human claim. What the test guarantees is that nothing is missing or
# invented; that `crf` really is an int is asserted by the same care as any other comment.

from typing import Callable

__version__: str
build_manifest: dict[str, str]

# --- enumerations -----------------------------------------------------------
#
# These are pybind11 enumerations, not enum.Enum: the members are attributes of the class
# and carry `name` and `value`, but the class does not inherit from the standard library's.
# Declared as they behave rather than as they would look nicer.

class Backend:
    AUTO: Backend
    NVIDIA_NVENC: Backend
    INTEL_ONE_VPL: Backend
    VAAPI: Backend
    X264: Backend
    X265: Backend
    X264_DIRECT: Backend
    X265_DIRECT: Backend
    INTEL_ONE_VPL_DIRECT: Backend
    __members__: dict[str, Backend]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class Codec:
    H264: Codec
    H265: Codec
    SAME_AS_SOURCE: Codec
    __members__: dict[str, Codec]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class PixelFormat:
    UNKNOWN: PixelFormat
    YUV420P: PixelFormat
    YUV422P: PixelFormat
    YUV444P: PixelFormat
    NV12: PixelFormat
    P010: PixelFormat
    GRAY8: PixelFormat
    HARDWARE_SURFACE: PixelFormat
    __members__: dict[str, PixelFormat]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class ObjectPriority:
    BACKGROUND: ObjectPriority
    LOW: ObjectPriority
    NORMAL: ObjectPriority
    HIGH: ObjectPriority
    CRITICAL: ObjectPriority
    __members__: dict[str, ObjectPriority]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class LogLevel:
    TRACE: LogLevel
    DEBUG: LogLevel
    INFO: LogLevel
    WARNING: LogLevel
    ERROR: LogLevel
    __members__: dict[str, LogLevel]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class SidecarFormat:
    NONE: SidecarFormat
    NDJSON: SidecarFormat
    __members__: dict[str, SidecarFormat]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class EncoderState:
    CREATED: EncoderState
    CONFIGURED: EncoderState
    RUNNING: EncoderState
    DRAINING: EncoderState
    END_OF_STREAM: EncoderState
    STOPPED: EncoderState
    FAILED: EncoderState
    __members__: dict[str, EncoderState]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class StatusCode:
    OK: StatusCode
    AGAIN: StatusCode
    END_OF_STREAM: StatusCode
    INVALID_ARGUMENT: StatusCode
    UNSUPPORTED: StatusCode
    DEVICE_UNAVAILABLE: StatusCode
    BACKEND_ERROR: StatusCode
    OUT_OF_MEMORY: StatusCode
    TIMEOUT: StatusCode
    ERROR: StatusCode
    __members__: dict[str, StatusCode]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

# --- geometry and timing ----------------------------------------------------

class Rational:
    num: int
    den: int
    def __init__(self, num: int = ..., den: int = ...) -> None: ...

class MediaTimestamp:
    value: int
    time_base: Rational
    def __init__(self, value: int = ..., time_base: Rational = ...) -> None: ...
    def valid(self) -> bool: ...

class Point:
    x: float
    y: float
    def __init__(self) -> None: ...

class TrackEvidence:
    present: bool
    age_sec: float
    speed: float
    sample_count: int
    def __init__(self) -> None: ...

class BoundingBox:
    t_sec: float
    kind: str
    x: float
    y: float
    width: float
    height: float
    track_id: int
    confidence: float
    label: str
    polygon: list[Point]
    track: TrackEvidence
    def __init__(self) -> None: ...

class FrameMetadata:
    timestamp: MediaTimestamp
    force_key_frame: bool
    objects: list[BoundingBox]
    def __init__(self) -> None: ...

# --- configuration ----------------------------------------------------------

class InputVideoFormat:
    width: int
    height: int
    pixel_format: PixelFormat
    frame_rate: Rational
    time_base: Rational
    def __init__(self) -> None: ...
    def valid(self) -> bool: ...

class BackendSelection:
    preferred: list[Backend]
    allow_unlisted: bool
    def __init__(self) -> None: ...

class KindQuality:
    qp_delta: int
    pad: float
    def __init__(self) -> None: ...

class Quality:
    kinds: dict[str, KindQuality]
    background_qp_delta: int
    pad: float
    margin_px: int
    min_roi_side: int
    max_regions: int
    max_protected_fraction: float
    hold_sec: float
    mask_levels: int
    mask_qp_delta_at_255: int
    def __init__(self) -> None: ...

class TemporalROIConfig:
    enabled: bool
    max_missing_frames: int
    max_hold_sec: float
    confidence_decay: float
    spatial_smoothing: float
    temporal_smoothing: float
    max_qp_delta_per_frame: int
    use_track_prediction: bool
    def __init__(self) -> None: ...

class RateControl:
    target_bitrate: int
    max_average_overshoot: float
    max_peak_overshoot: float
    recovery_window_ms: int
    def __init__(self) -> None: ...

class MetadataConfig:
    bitstream_sei: bool
    max_objects: int
    sidecar: SidecarFormat
    # Called once per emitted packet, in emission order, with one complete record and no
    # trailing newline (§19.3).
    sidecar_sink: Callable[[str], None] | None
    def __init__(self) -> None: ...

class Settings:
    encoder: Backend
    backend_selection: BackendSelection
    codec: Codec
    crf: int
    gop: int
    quality: Quality
    temporal: TemporalROIConfig
    rate_control: RateControl
    metadata: MetadataConfig
    def __init__(self) -> None: ...

class EncoderConfig:
    input: InputVideoFormat
    settings: Settings
    # §29. Left unset, diagnostics go to stderr one line each, as they do in C++. A sink
    # that raises does not unwind into the encoder: the exception is caught and reported on
    # stderr instead.
    log: Callable[[LogLevel, str], None] | None
    def __init__(self) -> None: ...

# --- what comes back --------------------------------------------------------

class EncoderCapabilities:
    h264: bool
    h265: bool
    roi_rectangles: bool
    qp_map: bool
    qp_map_block_width: int
    qp_map_block_height: int
    ctu_qp: bool
    emphasis_map: bool
    runtime_roi_update: bool
    sei: bool
    native_bitrate: bool
    main10: bool
    p010_input: bool
    max_roi_regions: int
    def __init__(self) -> None: ...

class EncoderStatistics:
    def __init__(self) -> None: ...
    @property
    def frames_submitted(self) -> int: ...
    @property
    def packets_received(self) -> int: ...
    @property
    def bytes_encoded(self) -> int: ...
    @property
    def frames_converted(self) -> int: ...
    @property
    def frames_uploaded(self) -> int: ...
    @property
    def regions_dropped(self) -> int: ...
    @property
    def backends_declined(self) -> int: ...
    @property
    def fallbacks_applied(self) -> int: ...
    @property
    def encoder_used(self) -> Backend: ...
    @property
    def frames_quantiser_clamped(self) -> int: ...
    @property
    def cpu_h265_target_guard(self) -> bool: ...
    @property
    def bitrate_target_unreachable(self) -> bool: ...
    @property
    def keyframes_requested(self) -> int: ...
    @property
    def backend_name(self) -> str: ...
    @property
    def roi_lag_measured(self) -> bool: ...
    @property
    def roi_lag_samples(self) -> int: ...
    @property
    def roi_lag_min_iou(self) -> float: ...
    @property
    def roi_lag_p95_frames(self) -> float: ...
    @property
    def roi_lag_p99_frames(self) -> float: ...
    @property
    def roi_lag_p95_ms(self) -> float: ...
    @property
    def roi_lag_p99_ms(self) -> float: ...
    @property
    def roi_lag_acceptance(self) -> bool: ...

class EncodedPacket:
    @property
    def data(self) -> bytes: ...
    @property
    def pts(self) -> int: ...
    @property
    def dts(self) -> int: ...
    @property
    def duration(self) -> int: ...
    @property
    def time_base(self) -> Rational: ...
    @property
    def key_frame(self) -> bool: ...
    @property
    def codec(self) -> Codec: ...

class Frame:
    def valid(self) -> bool: ...

# --- the encoder ------------------------------------------------------------

class SmartVideoEncoder:
    def __init__(self, config: EncoderConfig) -> None: ...
    def __enter__(self) -> SmartVideoEncoder: ...
    def __exit__(self, *args: object) -> None: ...
    # False means the input queue is full (§24's backpressure): drain and try again.
    def submit_frame(self, frame: Frame, metadata: FrameMetadata) -> bool: ...
    def receive_available(self) -> list[EncodedPacket]: ...
    def flush(self) -> None: ...
    def close(self) -> None: ...
    def get_capabilities(self) -> EncoderCapabilities: ...
    def get_statistics(self) -> EncoderStatistics: ...
    @property
    def at_end(self) -> bool: ...
    @property
    def closed(self) -> bool: ...

# --- exceptions (§58.6) -----------------------------------------------------

class SmartVideoEncoderError(Exception):
    # The StatusCode the C++ side returned, preserved rather than flattened into a message
    # (§58.6). An int, because it is the enumerator's value; status_name is its spelling.
    status_code: int
    status_name: str
class InvalidArgumentError(SmartVideoEncoderError): ...
class UnsupportedError(SmartVideoEncoderError): ...
class DeviceUnavailableError(SmartVideoEncoderError): ...
class BackendError(SmartVideoEncoderError): ...

# --- module-level functions -------------------------------------------------

def available_backends() -> list[Backend]: ...
def backend_name(backend: Backend) -> str: ...
def get_capabilities(backend: Backend, codec: Codec) -> EncoderCapabilities: ...
def status_code_name(code: StatusCode) -> str: ...
def exception_for_status_code(
    code: StatusCode) -> type[SmartVideoEncoderError] | None: ...
def log_level_name(level: LogLevel) -> str: ...
