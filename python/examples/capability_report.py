#!/usr/bin/env python3
"""What this build can do on this machine -- TZ v4.2 §58.3 and §58.10.9.

§58.3 asks for capability discovery that does not require opening an encoder, and this is
why it matters: choosing a backend by name and finding out afterwards that it cannot do
what you need is a failed encode, while asking first is a branch. Nothing below opens an
encoder or touches a frame.

The capabilities are reported per codec rather than once per backend, because on Intel
silicon the region limit is four for H.264 and eight for H.265 -- measured, not assumed. A
flat report would have to pick one of those and be wrong about the other.

The C++ tool sve-capabilities answers the same question for a deployment that has no Python
in it (§23); this is the same information through the wrapper.
"""

import smart_video_encoder as sve

CODECS = [("h264", sve.Codec.H264), ("h265", sve.Codec.H265)]


def main() -> None:
    print(f"smart-video-encoder {sve.__version__}")
    print(f"  built from  : {sve.build_manifest['commit']}")
    print(f"  libx264     : {sve.build_manifest['libx264'] or '(not in this build)'}")
    print(f"  libx265     : {sve.build_manifest['libx265'] or '(not in this build)'}")
    print(f"  licence     : {sve.build_manifest['license']}")
    print()

    backends = sve.available_backends()
    if not backends:
        # Not a crash and not an empty report: a build with no backend can encode nothing,
        # and saying so is the answer.
        print("no backends in this build")
        return

    for backend in backends:
        print(sve.backend_name(backend))
        produced_something = False
        for name, codec in CODECS:
            caps = sve.get_capabilities(backend, codec)
            supported = caps.h264 if codec is sve.Codec.H264 else caps.h265
            if not supported:
                continue
            produced_something = True
            # 0 is "no limit it can name", not "no regions" -- the software backends take
            # as many as the caller has. Printed as a word so that it cannot be read as the
            # opposite of what it means.
            limit = "unlimited" if caps.max_roi_regions == 0 else str(caps.max_roi_regions)
            print(f"  {name}: regions={limit} "
                  f"roi_rect={caps.roi_rectangles} "
                  f"qp_map={caps.qp_map}({caps.qp_map_block_width}x"
                  f"{caps.qp_map_block_height}) "
                  f"sei={caps.sei} native_bitrate={caps.native_bitrate} "
                  f"main10={caps.main10}")
        if not produced_something:
            # Registered but unable to produce anything here -- no device, or no encoder in
            # this build. A backend missing from the list and a backend that cannot run are
            # different answers, so this one is said rather than omitted.
            print("  (registered, but produces no codec on this machine)")

    print()
    print("Picking one, without opening an encoder:")
    wanted = sve.Codec.H265
    for backend in backends:
        caps = sve.get_capabilities(backend, wanted)
        if caps.h265 and caps.qp_map:
            print(f"  per-block quality maps for H.265: {sve.backend_name(backend)}")
            break
    else:
        print("  nothing here offers a per-block quality map for H.265")


if __name__ == "__main__":
    main()
