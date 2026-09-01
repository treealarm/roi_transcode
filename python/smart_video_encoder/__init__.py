"""smart-video-encoder: adaptive ROI video encoding (TZ v4.2 §58).

The Python layer is thin by design (§58.1): encoding, ROI policy, temporal state, capability
mapping, metadata synchronisation and backend fallback all stay in the C++ core. What lives
here is type conversion, lifetime, exceptions and a Pythonic surface over them.

    import numpy as np
    import smart_video_encoder as sve

    cfg = sve.EncoderConfig()
    cfg.input.width, cfg.input.height = 640, 480
    cfg.input.pixel_format = sve.PixelFormat.YUV420P
    cfg.settings.codec = sve.Codec.H264
    cfg.settings.backend_selection.preferred = [sve.Backend.X264_DIRECT]

    with sve.SmartVideoEncoder(cfg) as enc:
        enc.submit_frame(sve.frame_from_planes(planes, sve.PixelFormat.YUV420P, 640, 480),
                         meta)
        packets = enc.receive_available()
        enc.flush()
        while not enc.at_end:
            packets += enc.receive_available()
"""

from ._core import (  # noqa: F401
    Backend,
    BackendSelection,
    BoundingBox,
    Codec,
    EncodedPacket,
    EncoderCapabilities,
    EncoderConfig,
    EncoderState,
    EncoderStatistics,
    Frame,
    FrameMetadata,
    InputVideoFormat,
    MediaTimestamp,
    MetadataConfig,
    ObjectPriority,
    PixelFormat,
    RateControl,
    Rational,
    Settings,
    SidecarFormat,
    SmartVideoEncoder,
    StatusCode,
    TemporalROIConfig,
    available_backends,
    backend_name,
    get_capabilities,
    status_code_name,
)
from ._core import (  # noqa: F401
    BackendError,
    DeviceUnavailableError,
    InvalidArgumentError,
    SmartVideoEncoderError,
    UnsupportedError,
)
from ._core import _frame_from_arrays

# §58.7: one version source. _core carries what the build read out of vcpkg.json, so the
# Python package cannot report a number the C++ library disagrees with.
from ._core import __version__  # noqa: F401

# §58.7's build manifest: what this particular artefact was built from. A wheel is opaque,
# and "which libx264 is in here, from which commit, under what licence" is the first thing
# a bug report needs.
from ._core import build_manifest  # noqa: F401

__all__ = [name for name in dir() if not name.startswith("_")]


def frame_from_planes(planes, pixel_format, width, height):
    """Wrap plane arrays as a Frame without copying (§58.4).

    `planes` is one array per plane, or a single array for a single-plane format. Each must
    be two-dimensional, of the dtype the format implies, and contiguous within a row; rows
    may be padded. Anything else raises ValueError or TypeError rather than being accepted
    and read wrongly later.

    The returned Frame holds a reference to the arrays, so they stay alive for as long as
    the encoder needs them -- which is past the submit that took them, since codecs buffer.
    """
    return _frame_from_arrays(planes, pixel_format, width, height)
