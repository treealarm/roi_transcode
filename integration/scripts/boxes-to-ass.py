#!/usr/bin/env python3
"""Turn a regions sidecar into ASS subtitles, so a player can draw the boxes over the clip.

The encoded file carries no boxes. It cannot: what ROI encoding does is spend bits differently,
and nothing about where they went survives into the bitstream (SEI metadata is not implemented,
deliberately -- see docs/plan-v2.md, block D). So the regions travel beside the video, and the
way to see them without writing a viewer is to hand them to one that already exists.

ASS is that hand-off. Its drawing mode (\\p1) takes vector shapes, every player worth using
renders it through libass, and the result composites over the video without touching a pixel of
it -- which matters, because a file with boxes burned in is no longer comparable to its source
and every quality metric computed against it is wrong.

    boxes-to-ass.py clip.boxes.json -o clip.ass
    mpv clip.roi.mp4 --sub-file=clip.ass
    vlc --sub-file=clip.ass clip.roi.mp4

The same .ass fits the source clip as well as the re-encoded one, which is how you compare what
was protected against what came out.

Input, as written by video_a's --roi-file pass:

    {"width": 1280, "height": 720, "fps": 30,
     "frames": [{"t": 1.2, "boxes": [{"kind": "face", "x": .., "y": .., "w": .., "h": ..,
                                      "track_id": 3, "confidence": 0.88}]}]}

Coordinates are normalized 0..1 from the top-left corner, the same convention Box carries on the
wire, so the rectangles land where the encoder was told to spend its bits and not merely near it.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ASS colours are BGR, not RGB. Written out per kind rather than generated, because these are read
# at a glance while scrubbing and the assignment wants to be stable between runs.
KIND_COLOURS = {
    "face": "&H5555FF&",           # red
    "person": "&H55FF55&",         # green
    "vehicle": "&HFFCC55&",        # cyan
    "license_plate": "&H55FFFF&",  # yellow
    "plate": "&H55FFFF&",
}
DEFAULT_COLOUR = "&HFFFFFF&"


def ass_time(seconds: float) -> str:
    seconds = max(0.0, seconds)
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    return f"{hours:d}:{minutes:02d}:{seconds % 60:05.2f}"


def rectangle(box: dict, width: int, height: int, thickness: int) -> list[str]:
    kind = str(box.get("kind", "object"))
    colour = KIND_COLOURS.get(kind, DEFAULT_COLOUR)

    x0 = int(round(float(box["x"]) * width))
    y0 = int(round(float(box["y"]) * height))
    x1 = int(round((float(box["x"]) + float(box.get("w", box.get("width", 0)))) * width))
    y1 = int(round((float(box["y"]) + float(box.get("h", box.get("height", 0)))) * height))
    if x1 <= x0 or y1 <= y0:
        return []

    track = box.get("track_id") or 0
    label = kind if not track else f"{kind}#{track}"

    # The outline is the shape's border (\3c) with the fill made fully transparent (\1a&HFF&).
    # Filling it would hide exactly the thing the box is pointing at.
    shape = (
        f"{{\\an7\\pos(0,0)\\p1\\bord{thickness}\\shad0"
        f"\\1a&HFF&\\3c{colour}\\3a&H00&}}"
        f"m {x0} {y0} l {x1} {y0} {x1} {y1} {x0} {y1}{{\\p0}}"
    )
    # Above the box where there is room, inside it where there is not -- a label clipped off the
    # top of the frame is the one case where it is needed most, on an object entering the shot.
    label_y = y0 - 18 if y0 >= 18 else y0 + 2
    caption = (
        f"{{\\an7\\pos({x0},{label_y})\\bord2\\shad0"
        f"\\1c{colour}\\3c&H000000&\\fs16}}{label}"
    )
    return [shape, caption]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sidecar", type=Path, help="the *.boxes.json written by the ROI pass")
    parser.add_argument("-o", "--output", type=Path, required=True, help="the .ass to write")
    parser.add_argument("--thickness", type=int, default=3, help="outline width in pixels")
    parser.add_argument("--hold", type=float, default=0.0,
                        help="extra seconds each frame's boxes stay on screen")
    args = parser.parse_args()

    doc = json.loads(args.sidecar.read_text(encoding="utf-8"))
    width = int(doc["width"])
    height = int(doc["height"])
    frames = doc.get("frames", [])
    if not frames:
        print("no frames in the sidecar", file=sys.stderr)
        return 1

    fps = float(doc.get("fps") or 0) or 25.0

    lines = [
        "[Script Info]",
        "ScriptType: v4.00+",
        # PlayResX/Y are what make the pixel coordinates below mean anything: libass scales the
        # whole script from this resolution to the video's, so a sidecar from a 720p source still
        # lands correctly when the same boxes are viewed against a rescaled copy.
        f"PlayResX: {width}",
        f"PlayResY: {height}",
        "WrapStyle: 2",
        "ScaledBorderAndShadow: yes",
        "",
        "[V4+ Styles]",
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding",
        "Style: Default,DejaVu Sans,16,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1",
        "",
        "[Events]",
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text",
    ]

    events = 0
    for i, frame in enumerate(frames):
        start = float(frame["t"])
        # Each frame's boxes stand until the next frame's replace them, so there is no gap between
        # them to flicker through. The last frame gets one frame period of its own.
        end = float(frames[i + 1]["t"]) if i + 1 < len(frames) else start + 1.0 / fps
        end = max(end, start + 1e-3) + args.hold
        for box in frame.get("boxes", []):
            for text in rectangle(box, width, height, args.thickness):
                lines.append(
                    f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Default,,0,0,0,,{text}"
                )
                events += 1

    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{args.output}: {events} events over {len(frames)} frames", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
