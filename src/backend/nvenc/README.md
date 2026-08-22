# NVENC backend — not implemented

TZ v2.0 §4 requires it. Deferred: there is no NVIDIA hardware to develop or test against,
and a backend written blind is worse than an absent one — §4 asks for the adaptive
mechanism to be chosen by capability query, which cannot be verified without the GPU.

The existing encoder already names `ENCODER_NVENC` and builds a qpDeltaMap for it; what is
missing is submitting that map to the SDK. See `src/core/roi_encoder.cpp`.
