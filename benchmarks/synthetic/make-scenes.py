#!/usr/bin/env python3
"""Synthetic scenes with exact ground truth.

Each scene is a clip plus a JSON saying where the object is on every frame. That
is what the frame mode needs to be tested honestly: a mask has to come from
somewhere, and a mask derived from the same guess the encoder is given proves
nothing.

The backgrounds differ in what they stress -- flat has no texture to hide
anything, edges show ringing, the zone plate sweeps spatial frequency, texture is
high-entropy, and moving is the only one where the box does not sit still.

Written in Python rather than as another shell script because the trajectory has
to be one definition: the same numbers produce the ffmpeg overlay expression and
the ground truth, and two copies of it would drift.
"""

import argparse
import json
import pathlib
import subprocess
import sys

W, H = 1280, 720
FPS = 25
DURATION = 12
FRAMES = FPS * DURATION

# Normalized plate geometry, matching testdata/synth_roi.json so the existing
# fixtures stay comparable.
PLATE_W, PLATE_H = 0.30, 0.20
STATIC_X, STATIC_Y = 0.35, 0.40

# The crowd scene's geometry, taken from this deployment's archive rather than chosen: a
# person box there averages 41.5% of the frame and a face 1.9%. That ratio is the whole
# reason the area budget exists, and no other scene here has it -- every one of them is a
# single object on 6% of the frame, which is why re-compression looked harmless on synthetic
# footage while it made real files 80% larger.
PERSON_W, PERSON_H = 0.45, 0.85     # 38% of the frame
PERSON_X, PERSON_Y = 0.28, 0.10
FACE_W, FACE_H = 0.11, 0.13         # 1.4% of the frame, and inside the person
FACE_X = PERSON_X + (PERSON_W - FACE_W) / 2
FACE_Y = PERSON_Y + 0.04

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# (lavfi source, filters applied after it). The duration and rate belong to the
# source, not to the chain -- appending them to a filtergraph is silently wrong.
BACKGROUNDS = {
    # Nothing to hide behind: measures the ROI mechanism on its own.
    "flat": ("color=c=0x808080:s={W}x{H}", "format=yuv420p"),
    # Hard bars. Ringing and blocking show up here first.
    "edges": (
        "color=c=black:s={W}x{H}",
        "format=gray,geq=lum='if(lt(mod(X\\,64)\\,32)\\,235\\,16)',format=yuv420p",
    ),
    # Spatial frequency sweep: which frequencies the encoder destroys.
    "zoneplate": ("zoneplate=s={W}x{H}:k0=64", "format=yuv420p"),
    # High-entropy grid, the background the original fixture used.
    "texture": (
        "color=c=0x3a3a3a:s={W}x{H}",
        "format=yuv420p,"
        "geq=lum='if(lt(mod(X\\,32)\\,3)\\,235\\,if(lt(mod(Y\\,32)\\,3)\\,235\\,70))'"
        ":cb=128:cr=128,"
        "drawgrid=width=128:height=128:thickness=3:color=0xffff00@0.95",
    ),
    # Incoherent motion everywhere, and none of it worth keeping: temporal noise over the
    # texture, standing in for foliage in wind, rain, or sensor grain.
    #
    # This is the scene the other four cannot produce. On all of them the only thing that moves
    # is the plate, which is the best case for an encoder's own rate control -- it spends bits
    # on what changed, and what changed is what matters, so region hints have little left to
    # add. Here the encoder's instinct is exactly wrong: motion compensation cannot predict
    # noise, so every frame pays for the background afresh, while the stationary plate codes
    # almost free and stays as coarse as the first frame left it.
    #
    # allf=t makes the noise differ per frame; without it the grain is fixed and the scene is
    # just a busier texture with nothing moving at all.
    "distraction": (
        "color=c=0x3a3a3a:s={W}x{H}",
        "format=yuv420p,"
        "geq=lum='if(lt(mod(X\\,32)\\,3)\\,235\\,if(lt(mod(Y\\,32)\\,3)\\,235\\,70))'"
        ":cb=128:cr=128,"
        "noise=alls=14:allf=t",
    ),
}

# name -> (background, moving?, subject)
SCENES = {
    "flat": ("flat", False, "plate"),
    "edges": ("edges", False, "plate"),
    "zoneplate": ("zoneplate", False, "plate"),
    "texture": ("texture", False, "plate"),
    "moving": ("texture", True, "plate"),
    # Plate static on purpose: the point is that everything else moves.
    "distraction": ("distraction", False, "plate"),
    # Two objects, nested, with the size ratio real footage has. Static, because the variable
    # under test is area, not motion -- and the person is filled with per-frame noise, so it
    # costs bits to protect whether or not it moves.
    #
    # The background is the noisy one on purpose. Over a static texture there is nothing to
    # save by penalising the background, so even a correct budget comes out larger than a flat
    # re-encode and the scene would answer a question nobody asked. Real footage pays most of
    # its bits for the background, which is exactly what makes re-compression worth doing.
    "crowded": ("distraction", False, "crowd"),
}


def plate_position(t):
    """Normalized top-left of the plate at time `t`. The one definition."""
    if t is None:  # static
        return STATIC_X, STATIC_Y
    # Left to right across the middle, ending short of the right edge.
    span = 1.0 - PLATE_W - 0.10
    return 0.05 + span * (t / DURATION), STATIC_Y


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr[-2000:])
        raise SystemExit(f"ffmpeg failed: {' '.join(cmd[:6])} ...")


def build_crowd(name, out_dir, background):
    """A person on nearly half the frame with a face inside them.

    The person is filled with per-frame noise on purpose: a flat panel codes almost free
    after the first frame, and protecting something that costs nothing proves nothing. Noise
    cannot be predicted, so every frame pays for it again -- which is what makes the area
    budget's decision visible in the file size.
    """
    source = out_dir / "source.mp4"
    pw, ph = int(round(PERSON_W * W)), int(round(PERSON_H * H))
    fw, fh = int(round(FACE_W * W)), int(round(FACE_H * H))
    px, py = int(round(PERSON_X * W)), int(round(PERSON_Y * H))
    fx, fy = int(round(FACE_X * W)), int(round(FACE_Y * H))

    bg_src, bg_chain = BACKGROUNDS[background]
    bg_src = bg_src.format(W=W, H=H)

    # Light enough to pass the ground-truth luma check against this background, which
    # compares the object against a strip beside it.
    person_chain = (
        "format=yuv420p,noise=alls=20:allf=t,"
        f"drawbox=x=0:y=0:w={pw}:h={ph}:color=black:t=6"
    )
    face_chain = (
        "format=yuv420p,"
        f"drawbox=x=0:y=0:w={fw}:h={fh}:color=black:t=4,"
        f"drawtext=fontfile={FONT}:text='FACE':fontsize=28:"
        "fontcolor=black:x=(w-text_w)/2:y=(h-text_h)/2"
    )

    run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", f"{bg_src}:d={DURATION}:r={FPS}",
        "-f", "lavfi", "-i", f"color=c=0xd2d2d2:s={pw}x{ph}:d={DURATION}:r={FPS}",
        "-f", "lavfi", "-i", f"color=c=0xf0f0f0:s={fw}x{fh}:d={DURATION}:r={FPS}",
        "-filter_complex",
        f"[0:v]{bg_chain}[bg];[1:v]{person_chain}[pe];[2:v]{face_chain}[fa];"
        f"[bg][pe]overlay=x={px}:y={py}[t1];"
        f"[t1][fa]overlay=x={fx}:y={fy},format=yuv420p",
        "-c:v", "libx264", "-preset", "fast", "-crf", "16", "-g", "50",
        "-frames:v", str(FRAMES), str(source),
    ])

    objects = [
        {"id": 1, "class": "person",
         "bbox": [px, py, pw, ph],
         "bbox_norm": [PERSON_X, PERSON_Y, PERSON_W, PERSON_H]},
        {"id": 2, "class": "face",
         "bbox": [fx, fy, fw, fh],
         "bbox_norm": [round(FACE_X, 6), round(FACE_Y, 6), FACE_W, FACE_H]},
    ]
    frames = [{"index": i, "t": round(i / FPS, 6), "objects": objects}
              for i in range(FRAMES)]
    (out_dir / "truth.json").write_text(json.dumps({
        "scene": name,
        "width": W, "height": H, "fps": FPS, "frame_count": FRAMES,
        "frames": frames,
    }, indent=1))

    # No `kinds`: the point of this scene is the importance model of section 11 deciding,
    # exactly as the ta_vms archive pass leaves it. Background at +6 for the same reason --
    # that is the number the archive pass uses, not the +20 the plate fixtures exaggerate to.
    (out_dir / "roi.json").write_text(json.dumps({
        "background_qp_delta": 6,
        "hold_sec": DURATION * 2,
        "samples": [
            {"t": 0, "kind": "person", "bbox_x": PERSON_X, "bbox_y": PERSON_Y,
             "bbox_w": PERSON_W, "bbox_h": PERSON_H, "track_id": 1, "confidence": 0.9},
            {"t": 0, "kind": "face", "bbox_x": round(FACE_X, 6), "bbox_y": round(FACE_Y, 6),
             "bbox_w": FACE_W, "bbox_h": FACE_H, "track_id": 2, "confidence": 0.6},
        ],
    }, indent=1))

    print(f"{name}: {source} ({FRAMES} frames, person {PERSON_W * PERSON_H:.0%} "
          f"of frame, face {FACE_W * FACE_H:.1%})")


def build_scene(name, out_dir):
    background, moving, subject = SCENES[name]
    out_dir.mkdir(parents=True, exist_ok=True)
    if subject == "crowd":
        build_crowd(name, out_dir, background)
        return
    source = out_dir / "source.mp4"

    pw, ph = int(round(PLATE_W * W)), int(round(PLATE_H * H))
    bg_src, bg_chain = BACKGROUNDS[background]
    bg_src = bg_src.format(W=W, H=H)

    if moving:
        span = 1.0 - PLATE_W - 0.10
        x_expr = f"(0.05+{span}*t/{DURATION})*{W}"
    else:
        x_expr = str(int(round(STATIC_X * W)))
    y_pos = int(round(STATIC_Y * H))

    # The plate itself: a light panel with a black border and a readable code, so
    # "did the zone survive" is answerable by eye as well as by SSIM.
    plate_src = f"color=c=0xe8e8e8:s={pw}x{ph}"
    plate_chain = (
        f"format=yuv420p,"
        f"drawbox=x=0:y=0:w={pw}:h={ph}:color=black:t=8,"
        f"drawtext=fontfile={FONT}:text='A123BC 77':fontsize=48:"
        f"fontcolor=black:x=(w-text_w)/2:y=(h-text_h)/2-10,"
        f"drawtext=fontfile={FONT}:text='RUS 12 345 678':fontsize=16:"
        f"fontcolor=black:x=(w-text_w)/2:y=h-36"
    )

    run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", f"{bg_src}:d={DURATION}:r={FPS}",
        "-f", "lavfi", "-i", f"{plate_src}:d={DURATION}:r={FPS}",
        "-filter_complex",
        f"[0:v]{bg_chain}[bg];[1:v]{plate_chain}[pl];"
        f"[bg][pl]overlay=x='{x_expr}':y={y_pos},format=yuv420p",
        "-c:v", "libx264", "-preset", "fast", "-crf", "16", "-g", "50",
        "-frames:v", str(FRAMES), str(source),
    ])

    # Ground truth: where the object is on every frame, in pixels and normalized.
    frames = []
    for i in range(FRAMES):
        t = i / FPS
        x, y = plate_position(t if moving else None)
        frames.append({
            "index": i,
            "t": round(t, 6),
            "objects": [{
                "id": 1,
                "class": "plate",
                "bbox": [
                    int(round(x * W)), int(round(y * H)),
                    int(round(PLATE_W * W)), int(round(PLATE_H * H)),
                ],
                "bbox_norm": [round(x, 6), round(y, 6), PLATE_W, PLATE_H],
            }],
        })
    (out_dir / "truth.json").write_text(json.dumps({
        "scene": name,
        "width": W, "height": H, "fps": FPS, "frame_count": FRAMES,
        "frames": frames,
    }, indent=1))

    # A roi.json the CLI can take directly. A static object needs one sample; a
    # moving one needs enough that the hold window always covers a frame.
    if moving:
        step = 0.2
        samples = []
        t = 0.0
        while t <= DURATION:
            x, y = plate_position(t)
            samples.append({"t": round(t, 3), "kind": "plate",
                            "bbox_x": round(x, 6), "bbox_y": round(y, 6),
                            "bbox_w": PLATE_W, "bbox_h": PLATE_H})
            t += step
        hold = step  # samples are denser than the window, so every frame is covered
    else:
        samples = [{"t": 0, "kind": "plate", "bbox_x": STATIC_X,
                    "bbox_y": STATIC_Y, "bbox_w": PLATE_W, "bbox_h": PLATE_H}]
        hold = DURATION * 2

    (out_dir / "roi.json").write_text(json.dumps({
        "background_qp_delta": 20,
        "hold_sec": hold,
        "kinds": {"plate": {"qp_delta": -8, "pad_norm": 0.05}},
        "samples": samples,
    }, indent=1))

    print(f"{name}: {source} ({FRAMES} frames, {len(samples)} roi samples)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scenes", nargs="*", default=None,
                    help="scene names; default is all of them")
    ap.add_argument("--out", default=None, help="output directory")
    args = ap.parse_args()

    # Three levels up: this file is benchmarks/synthetic/make-scenes.py, and testdata/ sits
    # at the project root beside it. Two levels landed in benchmarks/testdata/, which is not
    # where check.sh looks -- and it looked right for as long as somebody had once passed
    # --out by hand.
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    out = pathlib.Path(args.out) if args.out else root / "testdata" / "scenes"
    for name in (args.scenes or sorted(SCENES)):
        if name not in SCENES:
            raise SystemExit(f"unknown scene: {name} (have {', '.join(sorted(SCENES))})")
        build_scene(name, out / name)


if __name__ == "__main__":
    main()
