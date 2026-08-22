# syntax=docker/dockerfile:1.5
#
# C++ base for roi_transcode, separate from ta-deps on purpose.
#
# ta-deps serves media_server and video_a's analytics-worker; this serves the ROI
# encoder. Keeping them apart is not tidiness -- it is that neither side wants
# what the other needs. openvino is 143 minutes of the 234 ta-deps took to build
# and roi_transcode does not link it; qsv/vaapi and system libdrm are here and
# nowhere else, so ta-deps no longer carries anything GPU-related for a consumer
# that has no GPU.
#
# 24.04 rather than ta-deps' 22.04, matching the runtime image in Dockerfile.roi.
# glibc only works forwards -- a binary built against 2.39 will not run on 2.35 --
# so builder and runtime being the same release is what keeps that honest. It also
# means gcc-13 comes from the distro and needs no PPA.
#
# It does NOT align libva: vcpkg pins its own libva (2.24.1) regardless of the
# base, and ffmpeg[vaapi] depends on that port rather than on a system one. The
# runtime image ships vcpkg's libva alongside the binary for exactly that reason;
# see the comment in Dockerfile.roi.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV VCPKG_DEFAULT_BINARY_CACHE=/vcpkg/binary-cache
ENV CMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake

# Two of these are easy to lose and neither fails obviously:
#
# libdrm-dev has no vcpkg port at all -- the libva port's own portfile prints
# `sudo apt install libdrm-dev` and then fails at meson configure with an error
# that does not name the cause.
#
# python3 is what vcpkg builds its meson tool with, and meson is what libva
# builds with. ta-deps never lists it because software-properties-common (there
# for the gcc-13 PPA) drags it in; 24.04 needs no PPA, so here it has to be asked
# for by name.
RUN apt-get update && apt-get install -y \
    curl zip unzip tar git python3 \
    build-essential cmake ninja-build pkg-config nasm \
    libdrm-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg.git /vcpkg
RUN /vcpkg/bootstrap-vcpkg.sh

# Release-only triplet overlay (see triplets/x64-linux.cmake) — same triplet name, so every
# `:x64-linux` package suffix and downstream find_package() keeps resolving unchanged; only the
# install-time behavior (skip building the debug config) changes.
COPY triplets/ /vcpkg-overlay-triplets/

# The ffmpeg feature list is deliberately the one ta-deps used to carry, plus
# nothing: narrowing it would save a few minutes and change what the encoder can
# read, and the time that mattered was openvino's.
#
# Building qsv/vaapi is not enough to use them. The encoders live in libavcodec,
# but ROI on Intel also needs a current iHD driver in the runtime image and
# /dev/dri passed into the container. Without either, `--encoder auto` falls back
# to libx264 and everything still looks fine.
RUN --mount=type=cache,target=/vcpkg/buildtrees \
    --mount=type=cache,target=/vcpkg/packages \
    --mount=type=cache,target=/vcpkg/downloads \
    --mount=type=cache,target=/vcpkg/binary-cache \
    VCPKG_MAX_CONCURRENCY=1 \
    /vcpkg/vcpkg install \
        --overlay-triplets=/vcpkg-overlay-triplets \
        protobuf \
        grpc \
        nlohmann-json \
        ffmpeg[gpl,x264,x265,opus,vpx,aom,openssl,qsv,vaapi]:x64-linux
