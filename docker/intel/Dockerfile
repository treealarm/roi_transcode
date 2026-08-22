# roi-transcode-svc: ROI-aware re-encoding of finished archive clips.
#
# Separate from the media_server Dockerfile for one reason: this is the only
# service that touches the GPU, and its runtime image therefore needs a VA-API
# driver that the others have no use for.

FROM roi-deps AS builder

WORKDIR /build
COPY . /build

WORKDIR /build/roi_transcode

# Release, deliberately: roi-deps uses a release-only vcpkg overlay triplet, so a
# Debug configure would look for debug libraries that were never built.
# VCPKG_MANIFEST_MODE=OFF on purpose. vcpkg.json exists for consumers who build from a
# checkout (TZ v2.0 §48 requires manifest mode), but this image already carries the whole
# dependency tree installed classically -- that is what roi-deps is for. Leaving manifest
# mode on would make vcpkg reinstall everything into the build directory and throw away
# the base image's only reason to exist.
RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DSVE_BUILD_INTEGRATION=ON \
    -DSVE_BUILD_TESTS=OFF
RUN cmake --build build -j1 --target roi-transcode-svc

# libva travels with the binary, and that is not optional. vcpkg builds ffmpeg
# against its own libva (2.24), while Ubuntu 24.04 ships 2.20 -- and libva is
# linked dynamically, so the runtime's older one wins and the first hardware
# encode dies on `undefined symbol: vaMapBuffer2`. Lazily, too: the service
# starts fine and only fails once a job arrives.
#
# Shipping ours is safe in the direction that matters. libva looks up the
# driver's entry point by trying __vaDriverInit_<major>_<minor> downwards from
# its own version, so a newer libva loads the distro's older iHD; the reverse is
# what does not work.
RUN mkdir -p /runtime-libs && \
    cp $(g++ -print-file-name=libstdc++.so.6) /runtime-libs/ && \
    cp $(g++ -print-file-name=libgcc_s.so.1) /runtime-libs/ && \
    cp -P /vcpkg/installed/x64-linux/lib/libva.so.2* /runtime-libs/ && \
    cp -P /vcpkg/installed/x64-linux/lib/libva-drm.so.2* /runtime-libs/

# =========================
# Runtime
# =========================
# 24.04 rather than the 22.04 the other C++ image uses: the Intel media driver
# packaged there is the one this needs, and 22.04's is too old to encode at all.
# Its HEVC limitation does not bite us -- the archive is H.264, which is what
# this service is asked to produce.
FROM ubuntu:24.04

# libmfx-gen1.2 is the piece that is easy to miss. ffmpeg's h264_qsv talks to the
# oneVPL *dispatcher*, which is linked in statically from vcpkg -- but a
# dispatcher only finds implementations, it is not one. Without the GPU runtime
# installed here there is nothing for it to load, so QSV fails to open and the
# encoder chain quietly drops to VA-API. That still encodes on the iGPU, so the
# symptom is not an error: it is `--encoder auto` reporting vaapi where the whole
# point was qsv.
#
# vainfo and vpl-inspect (onevpl-tools) earn their size on a machine nobody can
# ssh into twice: between "the device is not passed through", "the group is
# wrong", "the driver did not load" and "no hardware implementation", every one
# of those looks identical from the outside -- a job that merely runs slower.
RUN apt-get update && apt-get install -y --no-install-recommends \
    intel-media-va-driver-non-free \
    libva2 \
    libva-drm2 \
    libmfx-gen1.2 \
    vainfo \
    onevpl-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/roi_transcode/build/integration/roi-transcode-svc /app/
COPY --from=builder /runtime-libs /app/

ENV LD_LIBRARY_PATH=/app
ENV LIBVA_DRIVER_NAME=iHD

CMD ["./roi-transcode-svc"]
