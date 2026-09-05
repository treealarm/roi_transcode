#!/usr/bin/env python3
"""A bench for judging ROI encoding by eye: original on the left, ours on the right.

The metrics this project already computes -- SSIM and PSNR over the zone, the background
and the ring around the edge -- answer whether the quality moved where it was asked to.
They do not answer whether the result looks right, and STATUS.md records that gap plainly
under Р1: VMAF is deferred, and "видно ли это глазу" has had no instrument at all.

No external dependencies, and that is deliberate rather than minimal. Р1 keeps heavy
dependencies out of the checking loop and Р2 keeps the tree portable, so a Qt or a pip
install would break both for a tool nobody needs to build. The standard library serves a
page; the browser is the toolkit.

It drives the roi-transcode CLI for now. run_encode is the one function that knows how the
library is invoked, so when §58's bindings land only it changes -- the page, the knobs and
the comparison do not.

    BUILD=build/integration python3 integration/bench/serve.py
    # then open http://127.0.0.1:8765

Needs ffmpeg and ffprobe on PATH, which every other check here needs too.

How it fits together
--------------------

Three processes, and the browser is not one of the interesting ones::

    browser  ──HTTP──▶  this script  ──subprocess──▶  roi-transcode  (the library)
                                     ──subprocess──▶  ffmpeg / ffprobe

The page is a single HTML string served from PAGE below; there is no build step, no
bundler and no framework, so what the browser runs is what is written here. It never
touches a file or a codec itself -- it can only ask this script, over five endpoints:

    GET  /             the page
    GET  /fixtures     the clips in the tree, each paired with its ROI document
    GET  /browse?dir=  one directory listed, for the file picker
    GET  /source?path= what a clip already is: codec, size, fps, frames, measured GOP
    GET  /roi?path=    a ROI document summarised (sample count, kinds, time span)
    GET  /detector     whether a detector can be run here, and what is missing when not
    GET  /ass?path=    the same boxes as ASS subtitles, for a player
    POST /detect       start a detection pass; returns a job id
    POST /run          start an encode; returns a job id straight away
    GET  /job?id=      that job's stage, and its result once done
    GET  /img/<run>/<src|out|diff>/<n>.png   one frame of one side

A run is a background thread rather than a blocking request, which is the whole reason
the page can show a stage while it works. encode_job is the sequence: read the source,
cut the segment, optionally burn frame numbers in, write the ROI document, run
roi-transcode, extract every frame of both sides in one ffmpeg pass each, then measure.
Each step sets a stage the page is polling for.

run_encode's descendant, the roi-transcode invocation inside encode_job, is the only
place that knows how the library is driven. §58's Python bindings replace that one
subprocess call; the endpoints, the page and the comparison do not change.

Nothing is written into the tree. Cuts, numbered copies, extracted frames and generated
ROI documents all live under WORK, a temporary directory removed when the server stops.

Where the boxes come from
-------------------------

Three sources, and the third is why this file knows about a sibling checkout at all. A box
drawn with the sliders answers "what does the encoder do"; a sidecar loaded from disk answers
it on a track that moves; neither answers it on a clip nobody has boxes for. video_a's
detector does, and `--detect-only` there is a pass with nothing on the other end -- decode,
detect, write `*.boxes.json`. No service is started and no frame goes over a socket: this
drives our own CLI from the sidecar afterwards, exactly as it does for a file picked by hand.

video_a stays optional. It is found through ANALYTICS_WORKER_BIN / VIDEO_A_DIR /
ANALYTICS_MODEL_PATH -- the names roi-clip.sh used before the service left this tree -- and
when it is not there the page greys the button out and says which of the two is missing.
"""

from __future__ import annotations

import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD", ROOT / "build" / "integration"))
CLI = BUILD / "roi-transcode"
PORT = int(os.environ.get("PORT", "8765"))

# Where the detector is, when there is one. video_a is a sibling checkout rather than a
# dependency: it holds the models and the detection pipeline, and this tree knows nothing about
# it beyond these three paths -- the same environment variables roi-clip.sh used before the
# service left. Absent, the page simply says so and everything else works.
VIDEO_A = Path(os.environ.get("VIDEO_A_DIR", ROOT.parent / "video_a"))
WORKER = Path(os.environ.get("ANALYTICS_WORKER_BIN", VIDEO_A / "build" / "analytics-worker"))
MODELS = Path(os.environ.get("ANALYTICS_MODEL_PATH", VIDEO_A / "models"))
TO_ASS = ROOT / "integration" / "scripts" / "boxes-to-ass.py"
DETECT_CLASSES = ("person", "face", "vehicle", "license_plate")

# What the file browser offers. Videos because that is the clip, and JSON because the same
# window picks a sidecar for the ROI field.
kVideoSuffixes = frozenset({".mp4", ".mkv", ".mov", ".avi", ".ts", ".webm", ".m4v", ".mpg"})
kDocSuffixes = frozenset({".json"})
# A directory with more entries than this is a listing nobody reads, and serialising it costs
# more than the answer is worth.
kMaxEntries = 2000
# What analytics-worker prints once per second of content in --detect-only. Matched rather than
# assumed: without it there is no progress at all, and with a changed format there is still a
# stage that says "detecting".
kDetectProgress = re.compile(r"detected through t=([0-9.]+)")

WORK = Path(tempfile.mkdtemp(prefix="sve-bench-"))
# Identifies this run of the server. The page checks it and says so when it no longer
# matches -- during a session of "edit, restart, reload" an open tab silently keeps running
# the old script, and a fix that is already on disk looks like a fix that does not work.
BUILD_ID = str(int(time.time()))
RUNS: dict[str, dict] = {}
JOBS: dict[str, dict] = {}
CLIPS: dict[tuple[str, float], Path] = {}
DETECTIONS: dict[tuple, Path] = {}
SOURCES: dict[str, dict] = {}
LOCK = threading.Lock()
COUNTER = [0]


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


def probe(path: Path, entries: str, stream: bool = True) -> str:
    cmd = ["ffprobe", "-v", "error"]
    if stream:
        cmd += ["-select_streams", "v:0"]
    cmd += ["-show_entries", entries, "-of", "csv=p=0", str(path)]
    return run(cmd).stdout.strip()


def as_number(text: str, default: float = 0.0) -> float:
    """ffprobe answers "N/A" as readily as it answers a number.

    Every field this asks for is optional in some container, and float("N/A") is a
    ValueError that surfaces far from the probe that caused it -- as a failed run with a
    message about string conversion, which is what it did.
    """
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


def frame_count(path: Path) -> int:
    """nb_read_packets is only filled in when ffprobe is told to count.

    Without -count_packets it reports N/A, and an int() of that is a zero that looks like a
    measurement -- which is exactly the trap check.sh documents beside its own copy.
    """
    out = run(["ffprobe", "-v", "error", "-count_packets", "-select_streams", "v:0",
               "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", str(path)])
    try:
        return int(out.stdout.strip().split(",")[0])
    except ValueError:
        return 0


def source_info(path: Path) -> dict:
    """What the clip already is, so a knob is set against a known starting point.

    The keyframe interval is measured rather than read: no container field states it, and
    "what GOP is the source" is the first thing anyone asks before changing --gop.
    """
    key = str(path)
    with LOCK:
        if key in SOURCES:
            return SOURCES[key]
    parts = probe(path, "stream=codec_name,width,height,r_frame_rate").split(",")
    frames = frame_count(path)
    keys = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-skip_frame", "nokey",
                "-show_entries", "frame=best_effort_timestamp_time", "-of", "csv=p=0",
                str(path)]).stdout.split()
    fps_text = parts[3] if len(parts) > 3 else "0/1"
    try:
        num, den = (int(x) for x in fps_text.split("/"))
        fps = num / den if den else 0.0
    except ValueError:
        fps = 0.0
    info = {
        "path": str(path),
        "codec": parts[0] if parts else "?",
        "width": int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0,
        "height": int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 0,
        "fps_text": fps_text,
        "fps": round(fps, 3),
        "frames": frames,
        "duration": as_number(probe(path, "format=duration", stream=False)),
        "keyframes": len(keys),
        # Kept, not just counted. A segment cut by stream copy starts at the keyframe at or
        # before the requested second, so this list is what says where a cut really begins --
        # and a box drawn against a start that is off by a GOP is a box in the wrong place.
        # csv=p=0 still prints the field separator, so every line here ends in a comma and
        # float() of it is a ValueError that as_number swallows into 0.0 -- which made every
        # cut believe it started at zero and put the boxes a window away from the object.
        "key_times": sorted(as_number(k.rstrip(",")) for k in keys),
        # Average, not a promise: a clip with scene cuts has no single GOP, and rounding
        # this to one number is what the field is for -- a starting point, not a claim.
        "gop": round(frames / len(keys)) if keys and frames else 0,
        "bytes": path.stat().st_size,
        "bit_rate": int(as_number(probe(path, "format=bit_rate", stream=False))),
        "settings": encoder_settings(path),
    }
    with LOCK:
        SOURCES[key] = info
    return info


def encoder_settings(path: Path) -> dict:
    """What the clip was encoded with, read out of the bitstream rather than guessed.

    CRF is an encoder-side decision and no container field carries it -- but both x264 and
    x265 write their whole option string into a user-data SEI, and that string is still
    there after muxing. So for anything either of them produced, "the same settings as the
    source" is a fact that can be read rather than a number to be matched by eye.

    Only the knobs this bench has are picked out. Anything else is left alone, and a clip
    from some other encoder simply yields nothing.
    """
    try:
        blob = path.read_bytes()[:1 << 20]
    except OSError:
        return {}
    for marker in (b"x264 - core", b"x265 [info]:"):
        at = blob.find(marker)
        if at < 0:
            continue
        text = blob[at:at + 2048].decode("latin-1")
        out: dict = {}
        for key, field in (("crf", "crf"), ("keyint", "gop"), ("bframes", "bframes")):
            for token in text.split():
                if token.startswith(key + "="):
                    value = token.split("=", 1)[1].rstrip(",;")
                    try:
                        out[field] = float(value) if key == "crf" else int(value)
                    except ValueError:
                        pass
                    break
        if out:
            out["encoder"] = "x264" if marker.startswith(b"x264") else "x265"
            return out
    return {}


def fixtures() -> list[dict]:
    """The clips in the tree, each already paired with the ROI document that belongs to it.

    Pairing is the point rather than the listing: testdata/scenes/moving/source.mp4 is only
    interesting alongside testdata/scenes/moving/roi.json, and typing both by hand is how
    you end up measuring one scene's boxes against another's picture.

    Nothing here is committed -- the fixtures are generated by make-synth.sh and
    make-scenes.py -- so a missing one is simply absent from the list rather than an error.
    """
    found = []
    synth = ROOT / "testdata" / "synth.mp4"
    if synth.exists():
        for roi in ("synth_roi.json", "roi.json"):
            if (ROOT / "testdata" / roi).exists():
                found.append({"label": f"synth ({roi})", "path": f"testdata/synth.mp4",
                              "roi": f"testdata/{roi}"})
    scenes = ROOT / "testdata" / "scenes"
    if scenes.is_dir():
        for d in sorted(p for p in scenes.iterdir() if p.is_dir()):
            clip, roi = d / "source.mp4", d / "roi.json"
            if clip.exists():
                found.append({"label": f"scene: {d.name}",
                              "path": str(clip.relative_to(ROOT)),
                              "roi": str(roi.relative_to(ROOT)) if roi.exists() else ""})
    fx = ROOT / "testdata" / "fixtures"
    if fx.is_dir():
        for d in sorted(p for p in fx.iterdir() if p.is_dir()):
            for name in ("video.mp4", "source.mp4"):
                clip = d / name
                if clip.exists():
                    roi = d / "roi.json"
                    found.append({"label": f"fixture: {d.name}",
                                  "path": str(clip.relative_to(ROOT)),
                                  "roi": str(roi.relative_to(ROOT)) if roi.exists() else ""})
                    break
    return found


def segment_start(info: dict, start: float) -> float:
    """Where a cut asked to begin at `start` will actually begin.

    Stream copy cannot begin anywhere: it starts at the keyframe at or before the requested
    second, and on this material a GOP is a second of content. So the number the page shows and
    the number the boxes are shifted by is this one -- the real start -- rather than the one
    that was typed.
    """
    keys = [k for k in info.get("key_times", []) if k <= start + 1e-6]
    return max(keys) if keys else 0.0


def clip_of(src: Path, start: float, seconds: float, total: float) -> tuple[Path, float]:
    """One window of the clip, and the second it really starts at.

    A window rather than a head, because an object need not be in the first two seconds: the
    clip that prompted this has its first vehicle at t=44. Stream-copied, so it is cheap and
    exact, and used as both the encoder's input and the reference it is compared against --
    so shortening the run does not tilt the comparison.
    """
    begin = segment_start(SOURCES.get(str(src), {}), start)
    if begin <= 0 and (seconds <= 0 or seconds >= total):
        return src, 0.0
    key = (str(src), begin, seconds)
    with LOCK:
        if key in CLIPS and CLIPS[key].exists():
            return CLIPS[key], begin
    dest = WORK / f"clip_{abs(hash(key)) & 0xFFFFFF:06x}.mp4"
    # -ss before -i so the demuxer seeks rather than decodes and drops; with -c copy it lands
    # on the keyframe segment_start named, which is why the two must agree.
    cmd = ["ffmpeg", "-hide_banner", "-v", "error", "-y"]
    if begin > 0:
        cmd += ["-ss", f"{begin:.6f}"]
    cmd += ["-i", str(src)]
    if seconds > 0:
        cmd += ["-t", str(seconds)]
    cmd += ["-c", "copy", str(dest)]
    proc = run(cmd)
    if proc.returncode != 0 or not dest.exists():
        return src, 0.0
    with LOCK:
        CLIPS[key] = dest
    return dest, begin


def numbered_clip(clip: Path) -> Path:
    """The same clip with its frame number burned into the picture.

    The point is not a caption. The number is drawn into the source before encoding, so it
    goes through the encoder and comes out the other side, and two panes showing the same
    number is evidence that they are the same picture rather than a promise that they are.
    Frames are selected by index precisely so they line up, and until now nothing on the
    page could show whether that had worked.

    Deliberately not baked into testdata/synth.mp4 by make-synth.sh. Three reasons, any one
    of them enough: check.sh samples the background at (40,40) in a 384x144 patch, so a
    number in the frame would sit inside a measured region; the fixture is built so the
    background does not crawl -- its own comments record the fight to keep P-frames
    skipping -- and a digit that changes every frame is exactly the moving detail that
    stops them; and every measurement recorded in STATUS.md was taken against that file.
    Done here instead, it costs the fixtures nothing and works on any clip, synthetic or
    real.

    Re-encoded losslessly (-qp 0), so what the ROI encoder sees is the source plus the
    number and nothing else lost on the way in.
    """
    # Into the run directory, never beside the source. When the segment covers the whole
    # clip, clip_of hands back the original path -- and the first version of this wrote its
    # numbered copy next to it, dropping testdata/synth_n.mp4 into the tree. *.mp4 is
    # gitignored, so nothing complained.
    dest = WORK / f"num_{abs(hash(str(clip))) & 0xFFFFFF:06x}.mp4"
    if dest.exists():
        return dest
    # Bottom left: out of the way of a centred ROI box, and out of the top-left corner the
    # rest of this project measures its background in.
    text = ("drawtext=text='%{n}':fontsize=44:fontcolor=white:borderw=2:bordercolor=black"
            ":box=1:boxcolor=black@0.55:x=14:y=h-th-14")
    proc = run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(clip),
                "-vf", text, "-c:v", "libx264", "-qp", "0", "-preset", "ultrafast",
                "-pix_fmt", "yuv420p", str(dest)])
    return dest if proc.returncode == 0 and dest.exists() else clip


def metric(kind: str, src: Path, enc: Path, crop: tuple[int, int, int, int]) -> float | None:
    x, y, w, h = crop
    filt = f"[0:v]crop={w}:{h}:{x}:{y}[a];[1:v]crop={w}:{h}:{x}:{y}[b];[a][b]{kind}"
    out = run(["ffmpeg", "-hide_banner", "-i", str(src), "-i", str(enc),
               "-filter_complex", filt, "-f", "null", "-"])
    marker = "All:" if kind == "ssim" else "average:"
    for line in reversed(out.stderr.splitlines()):
        if marker in line:
            try:
                return float(line.split(marker, 1)[1].split()[0])
            except ValueError:
                return None
    return None


def normalise_roi(doc: dict) -> dict:
    """Both sidecar shapes this tree writes, reduced to the one the CLI reads.

    roi_json.cpp requires samples[], one row per box with its own timestamp.
    boxes-to-ass.py consumes frames[], where one timestamp carries a list of boxes. The
    corner *.boxes.json files in the sample dataset are the second kind, so a bench that
    only understood the first would refuse exactly the fixtures with movement in them.
    """
    if "samples" in doc:
        return doc
    out = dict(doc)
    samples = []
    for frame in doc.get("frames", []):
        t = frame.get("t", 0.0)
        for box in frame.get("boxes", []):
            row = {"t": t, "kind": box.get("kind", "object")}
            for dst, keys in (("bbox_x", ("x", "bbox_x")), ("bbox_y", ("y", "bbox_y")),
                              ("bbox_w", ("w", "width", "bbox_w")),
                              ("bbox_h", ("h", "height", "bbox_h"))):
                for k in keys:
                    if k in box:
                        row[dst] = box[k]
                        break
            if "track_id" in box:
                row["track_id"] = box["track_id"]
            if "confidence" in box:
                row["confidence"] = box["confidence"]
            samples.append(row)
    out.pop("frames", None)
    out["samples"] = samples
    # The detector writes one row per decoded frame, carrying the last detection forward, and
    # this shape has no hold_sec of its own -- so the CLI would take its 0.25 s default and hold
    # every box for six frames at 25 fps. Six past positions protected, and six drawn. One frame
    # period is what a row-per-frame document means.
    if "hold_sec" not in out and as_number(str(doc.get("fps", 0))) > 0:
        out["hold_sec"] = 1.0 / float(doc["fps"])
    return out


def frames_of(doc: dict) -> dict:
    """The other direction: samples[] gathered back into one row per timestamp.

    boxes-to-ass.py reads frames[] and nothing else, and it is the only ASS generator in the
    tree -- writing a second one inside the bench is how two renderers of the same boxes start
    disagreeing. So a samples[] document is folded into the shape the script reads instead.
    """
    if "frames" in doc:
        return doc
    rows: dict[float, list[dict]] = {}
    for s in doc.get("samples", []):
        t = float(s.get("t", 0.0))
        rows.setdefault(t, []).append({
            "kind": s.get("kind", "object"),
            "x": float(s.get("bbox_x", s.get("x", 0.0))),
            "y": float(s.get("bbox_y", s.get("y", 0.0))),
            "w": float(s.get("bbox_w", s.get("w", 0.0))),
            "h": float(s.get("bbox_h", s.get("h", 0.0))),
            "track_id": s.get("track_id", 0),
        })
    return {"width": doc.get("width", 0), "height": doc.get("height", 0),
            "fps": doc.get("fps", 0),
            "frames": [{"t": t, "boxes": rows[t]} for t in sorted(rows)]}


def load_roi(path: Path) -> dict:
    return normalise_roi(json.loads(path.read_text()))


def track_of(doc: dict) -> list[dict]:
    """The submitted boxes, for drawing over the frames.

    Submitted, not applied: the library still grows each box by the halo, lifts it to
    min_roi_side and smooths it across frames (§11, §13), and reproducing that arithmetic
    here would draw something plausible that is not what the encoder used. What is drawn is
    what was handed in, which is also what §19.2 puts in the SEI.
    """
    rows = []
    for s in doc.get("samples", []):
        rows.append({
            "t": float(s.get("t", 0.0)), "kind": s.get("kind", "object"),
            "confidence": float(s.get("confidence", 0.0)),
            "x": float(s.get("bbox_x", s.get("x", 0.0))),
            "y": float(s.get("bbox_y", s.get("y", 0.0))),
            "w": float(s.get("bbox_w", s.get("w", 0.0))),
            "h": float(s.get("bbox_h", s.get("h", 0.0))),
        })
    return rows


def window_of(doc: dict, start: float, seconds: float) -> dict:
    """The document moved onto the segment's timeline, and cut to it.

    Sidecars are written against the source's own timeline -- the detector reads a whole clip
    and its boxes say t=44.2 -- while the encoder is handed a two-second cut whose first frame
    is t=0. Without this the boxes land a segment's worth of time away from the object.
    """
    if start <= 0 and seconds <= 0:
        return doc
    out = dict(doc)
    end = start + seconds if seconds > 0 else float("inf")
    rows = []
    for s in doc.get("samples", []):
        t = float(s.get("t", 0.0))
        if t + 1e-9 < start or t >= end:
            continue
        row = dict(s)
        row["t"] = round(t - start, 6)
        rows.append(row)
    out["samples"] = rows
    return out


def build_roi(p: dict, start: float = 0.0, seconds: float = 0.0) -> dict:
    """The ROI document for this run: either the file's, or one box from the knobs.

    A loaded file keeps its own geometry -- that is the point of loading it -- while the
    quality knobs still apply on top, because tuning them against a moving track is the
    reason to load one at all.
    """
    ref = (p.get("roi_file") or "").strip()
    if ref:
        path = Path(ref)
        if not path.is_absolute():
            path = ROOT / path
        doc = window_of(load_roi(path), start, seconds)
        if p.get("override_quality", True):
            doc["background_qp_delta"] = p["background_qp_delta"]
            kinds = {s["kind"] for s in doc.get("samples", []) if s.get("kind")}
            # Per kind, because a document with vehicles and plates in it is exactly the case
            # where one number is wrong: §11's ladder puts a plate at 1.00 and a vehicle at
            # 0.60, and a single slider applied to both said the car matters as much as the
            # number plate on it. The page sends a row per kind it found; anything it did not
            # send falls back to the shared slider.
            per_kind = p.get("kind_qp") or {}
            doc["kinds"] = {k: {"qp_delta": int(per_kind.get(k, p["qp_delta"])),
                                "pad_norm": p["pad_norm"]}
                            for k in (kinds or {p["kind"]})}
            # hold_sec is deliberately not overridden. It is the track's own timing, not a
            # quality knob: it says how long one sample stands before the next replaces it.
            # Overriding it with the slider's default of 20 s made every past position of a
            # moving box stay live -- the overlay drew a trail of them, and the encoder was
            # not drawing anything, it was protecting all 65 of them.
        return doc
    return {
        "background_qp_delta": p["background_qp_delta"],
        "hold_sec": p["hold_sec"],
        "kinds": {p["kind"]: {"qp_delta": p["qp_delta"], "pad_norm": p["pad_norm"]}},
        "samples": [{
            "t": p.get("t", 0.0), "kind": p["kind"],
            "bbox_x": p["bbox_x"], "bbox_y": p["bbox_y"],
            "bbox_w": p["bbox_w"], "bbox_h": p["bbox_h"],
        }],
    }


def detector_status() -> dict:
    """Whether a detector can be run at all, and what is missing when it cannot.

    The bench must work without video_a -- it is a sibling checkout, not a dependency of this
    tree -- so this answers rather than fails, and the page greys the button out with the reason
    instead of offering one that dies in a subprocess.
    """
    missing = []
    if not WORKER.exists():
        missing.append(f"no analytics-worker at {WORKER} (set ANALYTICS_WORKER_BIN)")
    if not MODELS.is_dir():
        missing.append(f"no models at {MODELS} (set ANALYTICS_MODEL_PATH)")
    return {"ok": not missing, "worker": str(WORKER), "models": str(MODELS),
            "classes": list(DETECT_CLASSES), "missing": missing}


def detect_job(job_id: str, p: dict) -> None:
    """Detections for one clip, written as the sidecar the encode then loads.

    Deliberately a separate job from the encode rather than a stage of it: inference costs
    seconds per second of content and does not depend on a single encoder knob, so tying it to
    the Encode button would re-run it on every change of crf. The result is cached by the clip
    and the detector's own settings, so pressing the button twice costs nothing.

    Runs over the whole source rather than over the cut segment, and that is the important
    choice. A detector run on a two-second head answers "nothing here" on any clip whose
    subject arrives later -- the one that prompted this has its first vehicle at t=44 of 96 --
    and it also gives boxes on a timeline that only that one cut understands. One pass over the
    source serves every window: sidecars are in the source's own time, and window_of moves them
    onto whichever segment is being encoded.
    """
    def stage(name: str) -> None:
        with LOCK:
            JOBS[job_id]["stage"] = name

    started = time.monotonic()
    try:
        status = detector_status()
        if not status["ok"]:
            raise RuntimeError("; ".join(status["missing"]))

        src = Path(p["source"])
        if not src.is_absolute():
            src = ROOT / src
        if not src.exists():
            raise RuntimeError(f"source not found: {src}")

        classes = [c for c in p.get("classes") or [] if c in DETECT_CLASSES]
        if not classes:
            raise RuntimeError("no detection classes selected")

        stage("reading source")
        info = source_info(src)

        detect_fps = float(p.get("detect_fps") or 2)
        confidence = float(p.get("min_confidence") or 0.5)
        key = (str(src), detect_fps, confidence, tuple(classes))
        with LOCK:
            dest = DETECTIONS.get(key)
        cached = dest is not None and dest.exists()
        if not cached:
            dest = WORK / f"det_{abs(hash(key)) & 0xFFFFFF:06x}.boxes.json"
            stage("detecting")
            # The worker takes these three from the environment and exits if any is missing --
            # they are its own contract, not ours, so whatever is already set wins.
            env = dict(os.environ)
            env["ANALYTICS_MODEL_PATH"] = str(MODELS)
            env.setdefault("ANALYTICS_DEVICE", "CPU")
            env.setdefault("ANALYTICS_REID_EMBED_INTERVAL_SEC", "15")
            cmd = [str(WORKER), "--roi-file", str(src), "--detect-only",
                   "--boxes", str(dest), "--detect-fps", str(detect_fps),
                   "--min-confidence", str(confidence), "--classes", ",".join(classes)]
            # Read as it runs rather than waiting for it. Inference over a minute of content
            # takes a minute of its own, and a stage that says "detecting" for all of it cannot
            # be told from a hang -- which is exactly what the first version looked like. The
            # worker reports the content second it has reached; the duration is known here, so
            # the two make a percentage.
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, env=env)
            tail: list[str] = []
            for line in proc.stdout or []:
                tail.append(line.rstrip())
                del tail[:-12]
                seen = kDetectProgress.search(line)
                if seen and info["duration"] > 0:
                    share = min(99, int(100 * float(seen.group(1)) / info["duration"]))
                    stage(f"detecting {share}% ({seen.group(1)} of "
                          f"{info['duration']:.0f} s)")
            if proc.wait() != 0 or not dest.exists():
                # The worker logs through spdlog to stderr and the useful line is the last one;
                # the whole log is a screen of model loading nobody asked for.
                raise RuntimeError(f"detector failed (rc={proc.returncode})\n"
                                   + "\n".join(tail))
            with LOCK:
                DETECTIONS[key] = dest

        raw = json.loads(dest.read_text())
        track = track_of(normalise_roi(raw))
        times = [b["t"] for b in track] or [0.0]
        with LOCK:
            JOBS[job_id].update(stage="done", done=True, result={
                "boxes": str(dest), "count": len(track), "cached": cached,
                "frames": len(raw.get("frames", [])),
                "kinds": sorted({b["kind"] for b in track}),
                "t_min": min(times), "t_max": max(times),
                "duration": info["duration"],
                "seconds": round(time.monotonic() - started, 2)})
    except Exception as exc:
        with LOCK:
            JOBS[job_id].update(stage="failed", done=True, error=str(exc))


def browse(raw: str) -> dict:
    """One directory, listed for the picker.

    Server-side because a browser file input hands over the file's contents and never its path,
    and the encoder needs the path. Read-only, and no wider than what this server already does:
    it encodes and probes any path it is given, and it listens on the loopback interface only.
    """
    shortcuts = [ROOT, Path.home(), Path("/")]
    for extra in filter(None, os.environ.get("BENCH_SHORTCUTS", "").split(":")):
        shortcuts.insert(1, Path(extra))
    roots = [str(d) for d in shortcuts if d.is_dir()]

    here = Path(raw).expanduser() if raw else ROOT
    if here.is_file():
        here = here.parent
    if not here.is_absolute():
        here = ROOT / here
    try:
        here = here.resolve()
        entries = sorted(here.iterdir(), key=lambda e: e.name.lower())[:kMaxEntries]
    except OSError as exc:
        return {"dir": str(here), "roots": roots, "dirs": [], "files": [],
                "error": f"cannot list {here}: {exc.strerror or exc}"}

    dirs, files = [], []
    for entry in entries:
        try:
            if entry.is_dir():
                dirs.append(entry.name)
            elif entry.suffix.lower() in kVideoSuffixes | kDocSuffixes:
                files.append({"name": entry.name, "size": entry.stat().st_size,
                              "video": entry.suffix.lower() in kVideoSuffixes})
        except OSError:
            continue                                  # a broken link is not worth an error page
    return {"dir": str(here), "parent": str(here.parent) if here.parent != here else "",
            "roots": roots, "dirs": dirs, "files": files}


def ass_of(doc: dict, width: int, height: int, thickness: int) -> str:
    """The boxes as ASS subtitles, through the script that already writes them.

    A player draws these over the clip without touching a pixel of it, which is what keeps the
    result comparable to its source -- boxes burned into the picture make every metric computed
    against it wrong. The bench draws its own overlay in SVG; this is the same boxes for
    everything that is not the bench.
    """
    frames = frames_of(doc)
    frames["width"] = int(frames.get("width") or width)
    frames["height"] = int(frames.get("height") or height)
    src = WORK / "ass_input.json"
    dest = WORK / "overlay.ass"
    src.write_text(json.dumps(frames))
    proc = run([sys.executable, str(TO_ASS), str(src), "-o", str(dest),
                "--thickness", str(thickness)])
    if proc.returncode != 0 or not dest.exists():
        raise RuntimeError((proc.stderr or "boxes-to-ass.py failed").strip())
    return dest.read_text()


def encode_job(job_id: str, p: dict) -> None:
    """One encode and everything measured about it, reporting where it is as it goes.

    Runs on its own thread so the page can show which stage is running. Before this the
    button simply sat there: a minute of silence and no way to tell a slow encode from a
    hung one.
    """
    def stage(name: str) -> None:
        with LOCK:
            JOBS[job_id]["stage"] = name

    started = time.monotonic()
    try:
        src = Path(p["source"])
        if not src.is_absolute():
            src = ROOT / src
        if not src.exists():
            raise RuntimeError(f"source not found: {src}")

        stage("reading source")
        info = source_info(src)

        stage("cutting segment")
        seconds = float(p.get("seconds") or 0)
        clip, begin = clip_of(src, float(p.get("from") or 0), seconds, info["duration"])
        # Measured before numbering. The numbered copy is lossless (-qp 0) and several
        # times the size of anything it is compared against, so reporting it as "input"
        # turned a 125% ratio into a 17% one and made the encoder look brilliant.
        segment_bytes = clip.stat().st_size
        if p.get("number_frames"):
            stage("numbering frames")
            # Used as both the encoder's input and the reference it is measured against,
            # so the number is in both panes and neither side is compared against a
            # picture the other never saw.
            clip = numbered_clip(clip)

        run_id = job_id
        rundir = WORK / run_id
        (rundir / "frames").mkdir(parents=True, exist_ok=True)
        roi_path, out_path = rundir / "roi.json", rundir / "out.mp4"
        doc = build_roi(p, begin, seconds)
        roi_path.write_text(json.dumps(doc, indent=2))

        cmd = [str(CLI), "--in", str(clip), "--roi", str(roi_path), "--out", str(out_path),
               "--encoder", p["encoder"], "--codec", p["codec"],
               "--crf", str(p["crf"]), "--gop", str(p["gop"])]
        if int(p.get("target_bitrate") or 0) > 0:
            cmd += ["--target-bitrate", str(int(p["target_bitrate"]))]

        stage("encoding")
        proc = run(cmd)
        log = (proc.stdout + proc.stderr).strip()
        if proc.returncode != 0 or not out_path.exists():
            raise RuntimeError(f"encode failed (rc={proc.returncode})\n{log}")

        width, height = info["width"], info["height"]
        track = track_of(doc)
        # Where to measure. With a loaded track the boxes move, so the zone is the
        # rectangle that contains every box in the run -- measuring at the slider position
        # instead would describe a rectangle the encoder never protected, which is how a
        # background can come out scoring better than the zone and nobody notices.
        if track and (p.get("roi_file") or "").strip():
            x0 = min(b["x"] for b in track); y0 = min(b["y"] for b in track)
            x1 = max(b["x"] + b["w"] for b in track)
            y1 = max(b["y"] + b["h"] for b in track)
        else:
            x0, y0 = p["bbox_x"], p["bbox_y"]
            x1, y1 = x0 + p["bbox_w"], y0 + p["bbox_h"]
        zw = max(16, int((x1 - x0) * width) // 2 * 2)
        zh = max(16, int((y1 - y0) * height) // 2 * 2)
        zx = max(0, min(int(x0 * width), width - zw))
        zy = max(0, min(int(y0 * height), height - zh))
        zone = (zx, zy, zw, zh)

        # A background patch the zone does not overlap, so "the background gave up quality"
        # is not read off pixels the zone was protecting. Corners in turn; the first that
        # clears the zone wins, and a zone covering the whole frame leaves the last one.
        bg = (16, 16, zw, zh)
        for cx, cy in ((16, 16), (width - zw - 16, 16), (16, height - zh - 16),
                       (width - zw - 16, height - zh - 16)):
            cx, cy = max(0, cx), max(0, cy)
            if cx + zw <= zx or cx >= zx + zw or cy + zh <= zy or cy >= zy + zh:
                bg = (cx, cy, zw, zh)
                break

        result = {
            "id": run_id, "bytes": out_path.stat().st_size,
            "clip_bytes": segment_bytes,
            "frames": frame_count(out_path), "clip_frames": frame_count(clip),
            "width": width, "height": height,
            "codec": probe(out_path, "stream=codec_name"),
            "fps_text": probe(out_path, "stream=r_frame_rate"),
            "duration": as_number(probe(out_path, "format=duration", stream=False)),
            "zone": zone, "bg": bg, "log": log,
            # Where the window really is. The cut snaps back to a keyframe, so this is not
            # what was typed into "from" and the page has to say which it is.
            "from": begin, "seconds_asked": seconds,
            "track": track_of(doc), "hold_sec": doc.get("hold_sec", 0.25),
            "fps": info["fps"],
            "ssim_zone": None, "ssim_bg": None, "psnr_zone": None,
            "flat_bytes": None, "flat_ssim_zone": None, "flat_ssim_bg": None,
        }
        with LOCK:
            RUNS[run_id] = {"src": clip, "out": out_path, "dir": rundir}
            JOBS[job_id]["result"] = dict(result)

        stage("extracting frames")
        for side in ("src", "out"):
            extract_all(RUNS[run_id], side, result["frames"])

        if p.get("flat_baseline"):
            # The same knobs with the ROI switched off. Input against output compares two
            # arbitrary encodes -- the source's own CRF against ours -- and answers nothing
            # about ROI. This is the question ROI exists to answer: at these settings, what
            # did steering the quality cost, and what did it buy in the zone.
            stage("encoding flat baseline")
            flat_doc = dict(doc)
            flat_doc["background_qp_delta"] = 0
            flat_doc["kinds"] = {k: {"qp_delta": 0, "pad_norm": 0.0}
                                 for k in doc.get("kinds", {"plate": {}})}
            flat_path, flat_out = rundir / "flat.json", rundir / "flat.mp4"
            flat_path.write_text(json.dumps(flat_doc, indent=2))
            flat_cmd = list(cmd)
            flat_cmd[flat_cmd.index("--roi") + 1] = str(flat_path)
            flat_cmd[flat_cmd.index("--out") + 1] = str(flat_out)
            if run(flat_cmd).returncode == 0 and flat_out.exists():
                result["flat_bytes"] = flat_out.stat().st_size
                if p.get("metrics", True):
                    result["flat_ssim_zone"] = metric("ssim", clip, flat_out, zone)
                    result["flat_ssim_bg"] = metric("ssim", clip, flat_out, bg)

        if p.get("metrics", True):
            stage("measuring")
            result["ssim_zone"] = metric("ssim", clip, out_path, zone)
            result["ssim_bg"] = metric("ssim", clip, out_path, bg)
            result["psnr_zone"] = metric("psnr", clip, out_path, zone)

        result["seconds"] = round(time.monotonic() - started, 2)
        with LOCK:
            JOBS[job_id].update(stage="done", done=True, result=result)
    except Exception as exc:  # any failure is the page's to show, not the console's
        with LOCK:
            JOBS[job_id].update(stage="failed", done=True, error=str(exc))


def extract_all(entry: dict, side: str, count: int) -> None:
    """Every frame of one side in a single ffmpeg run.

    Frame by frame each request re-opened and re-decoded the file, which is fine for
    stepping and hopeless for playback: at 25 fps the decode alone outruns the frame
    budget. One pass writes them all, so the slider is instant and Play has something to
    play.
    """
    frames = entry["dir"] / "frames"
    if (frames / f"{side}_{max(0, count - 1)}.png").exists():
        return
    run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(entry[side]),
         "-vsync", "0", "-start_number", "0", str(frames / f"{side}_%d.png")])


def extract(path: Path, index: int, dest: Path) -> bool:
    """Frame `index` exactly, by number rather than by time.

    Seeking by timestamp would be faster and wrong here: the source and the encode have
    different GOP structures, so a keyframe-accurate seek can land the two sides on
    different pictures -- and two different pictures side by side is the one thing this
    page must never show.
    """
    if dest.exists():
        return True
    proc = run(["ffmpeg", "-hide_banner", "-v", "error", "-i", str(path),
                "-vf", f"select=eq(n\\,{index})", "-vsync", "0", "-frames:v", "1",
                "-y", str(dest)])
    return proc.returncode == 0 and dest.exists()


def difference(entry: dict, index: int, gain: int) -> Path | None:
    """The two frames subtracted and amplified, because the eye is bad at small deltas.

    Compression loss at a sane CRF is a few levels per pixel: real, measurable, and
    invisible next to the picture it sits on. Subtracting and multiplying turns "I cannot
    see a difference" into a map of where the difference actually is -- which is usually
    the answer to whether the encoder spent the bits where it was told to.
    """
    src = entry["dir"] / "frames" / f"src_{index}.png"
    out = entry["dir"] / "frames" / f"out_{index}.png"
    if not extract(entry["src"], index, src) or not extract(entry["out"], index, out):
        return None
    dest = entry["dir"] / "frames" / f"diff_{index}_{gain}.png"
    if dest.exists():
        return dest
    proc = run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(src), "-i", str(out),
                "-filter_complex",
                # lutyuv rather than eq: eq's contrast stretches around mid-grey, which
                # pushes a difference near zero further towards black -- the amplifier has
                # to be a multiplier from zero, not a contrast curve.
                f"[0:v][1:v]blend=all_mode=difference,format=gray,"
                f"lutyuv=y=clip(val*{gain}\\,0\\,255)",
                str(dest)])
    return dest if proc.returncode == 0 and dest.exists() else None


PAGE = r"""<!doctype html>
<meta charset="utf-8">
<title>SVE bench</title>
<style>
  :root { color-scheme: light dark; --line:#8883; --bg:#fff; --fg:#111; --panel:#f6f6f7;
          --accent:#2f81f7; }
  @media (prefers-color-scheme: dark) { :root { --bg:#16171a; --fg:#e8e8ea; --panel:#1e2024; } }
  * { box-sizing:border-box; }
  body { margin:0; font:13px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
         background:var(--bg); color:var(--fg); display:flex; height:100vh; }
  #side { width:340px; flex:0 0 340px; overflow-y:auto; padding:14px; background:var(--panel);
          border-right:1px solid var(--line); }
  #main { flex:1; display:flex; flex-direction:column; min-width:0; padding:14px; gap:10px; }
  h1 { font-size:14px; margin:0 0 4px; }
  h2 { font-size:11px; text-transform:uppercase; letter-spacing:.08em; opacity:.6;
       margin:16px 0 6px; }
  label { display:flex; align-items:center; justify-content:space-between; gap:8px; margin:6px 0; }
  label span { white-space:nowrap; }
  input[type=range] { flex:1; min-width:0; }
  input[type=number], input[type=text], select { width:104px; padding:3px 5px;
       background:var(--bg); color:var(--fg); border:1px solid var(--line); border-radius:4px; }
  .val { width:54px; text-align:right; font-variant-numeric:tabular-nums; opacity:.75; }
  button { width:100%; padding:9px; margin-top:12px; font:inherit; font-weight:600;
           border:1px solid var(--line); border-radius:6px; background:var(--fg);
           color:var(--bg); cursor:pointer; }
  button:disabled { opacity:.5; cursor:default; }
  #srcinfo, #roiinfo { font-size:11.5px; opacity:.8; line-height:1.6; margin-top:4px; }
  #views { flex:1; display:flex; gap:10px; min-height:0; }
  .pane { flex:1; display:flex; flex-direction:column; min-width:0; }
  .pane h3 { font-size:11px; text-transform:uppercase; letter-spacing:.08em; opacity:.6;
             margin:0 0 5px; }
  .shot { flex:1; min-height:0; display:flex; align-items:center; justify-content:center; }
  .shot.zoomed { display:block; overflow:auto; }
  .shot.zoomed .frame { max-width:none; max-height:none; }
  .pane h3 em { font-style:normal; opacity:.6; font-weight:400; text-transform:none;
                letter-spacing:0; }
  #toolbar { display:flex; gap:18px; align-items:center; }
  button.mini { width:32px; margin:0; padding:5px 0; font-size:12px; }
  button.wide { margin-top:8px; padding:6px; font-weight:500; background:var(--bg);
                color:var(--fg); }
  .matchnote { font-size:11px; opacity:.6; margin:4px 0 0; }
  .inline { margin:0; gap:6px; }
  .inline select { width:auto; }
  .frame { position:relative; max-width:100%; max-height:100%; background:#0004;
           border:1px solid var(--line); border-radius:6px; overflow:hidden; }
  .frame img { display:block; width:100%; height:100%; object-fit:contain; }
  .frame svg { position:absolute; inset:0; width:100%; height:100%; pointer-events:none; }
  #bar { display:flex; align-items:center; gap:10px; }
  #prog { display:none; align-items:center; gap:8px; font-size:12px; }
  #prog.on { display:flex; }
  .spin { width:13px; height:13px; border:2px solid var(--line);
          border-top-color:var(--accent); border-radius:50%; animation:s .8s linear infinite; }
  @keyframes s { to { transform:rotate(360deg); } }
  #stats { display:flex; flex-wrap:wrap; gap:8px 20px; font-variant-numeric:tabular-nums; }
  #stats div { min-width:118px; }
  #stats b { display:block; font-size:10px; text-transform:uppercase; letter-spacing:.06em;
             opacity:.55; font-weight:600; }
  #log { white-space:pre-wrap; font:11px/1.4 ui-monospace,monospace; max-height:100px;
         overflow:auto; opacity:.65; border-top:1px solid var(--line); padding-top:8px; }
  #err { color:#c0392b; font-weight:600; white-space:pre-wrap; }
  /* A marker rather than a paragraph. The explanations are worth having and worth reading
     once; leaving them open cost more sidebar than the controls did. */
  .q { display:inline-flex; align-items:center; justify-content:center; width:13px;
       height:13px; margin-left:5px; border:1px solid var(--line); border-radius:50%;
       font-size:9px; font-style:normal; opacity:.55; cursor:help; vertical-align:middle; }
  .q:hover { opacity:1; border-color:var(--accent); color:var(--accent); }
  /* Fixed, and driven from script, because the sidebar scrolls: a tooltip positioned
     inside it is clipped by its own overflow, and this one has to sit outside. */
  #stale { position:fixed; top:0; left:0; right:0; z-index:10; padding:7px 12px;
           background:#f5a623; color:#111; font-size:12px; text-align:center; }
  #stale a { color:#111; }
  #tip { position:fixed; z-index:9; max-width:320px; padding:8px 10px; border-radius:6px;
         background:var(--fg); color:var(--bg); font-size:11.5px; line-height:1.45;
         box-shadow:0 4px 16px #0005; pointer-events:none; display:none; }
  /* The browse button sits inside the label beside its field, so it lines up with the input
     rather than taking a row of its own. */
  button.pick { width:26px; flex:0 0 26px; margin:0; padding:3px 0; font-weight:400;
                background:var(--bg); color:var(--fg); }
  #detstatus, #detinfo { font-size:11.5px; opacity:.8; margin-top:4px; }
  .warn { color:#b8860b; }
  .classes { display:flex; flex-wrap:wrap; gap:4px 12px; margin:6px 0; }
  .classes label { margin:0; gap:4px; justify-content:flex-start; }
  a.linkish { display:inline-block; margin-top:6px; font-size:11.5px; color:var(--accent); }
  /* The picker. Fixed and over everything, because the sidebar it is opened from scrolls and
     clips its own children. */
  #pickwrap { position:fixed; inset:0; z-index:11; background:#0007; display:none; }
  #pickwrap.on { display:flex; align-items:center; justify-content:center; }
  #picker { width:min(720px, 92vw); max-height:80vh; display:flex; flex-direction:column;
            background:var(--bg); border:1px solid var(--line); border-radius:8px;
            box-shadow:0 10px 40px #0007; }
  #picker header { display:flex; gap:8px; align-items:center; padding:10px 12px;
                   border-bottom:1px solid var(--line); }
  #pickpath { flex:1; min-width:0; }
  #picklist { overflow:auto; padding:4px 0; }
  #picklist div { padding:5px 12px; cursor:pointer; display:flex; justify-content:space-between;
                  gap:12px; }
  #picklist div:hover { background:var(--panel); }
  #picklist .dir { font-weight:600; }
  #picklist span { opacity:.55; font-variant-numeric:tabular-nums; }
  #pickroots { display:flex; gap:6px; flex-wrap:wrap; padding:8px 12px;
               border-top:1px solid var(--line); }
  #pickroots button { width:auto; margin:0; padding:4px 8px; font-weight:400; font-size:11px;
                      background:var(--bg); color:var(--fg); }
  .legend { display:flex; gap:14px; font-size:11px; opacity:.7; }
  .legend i { display:inline-block; width:20px; height:0; border-top:2px solid; margin-right:5px;
              vertical-align:middle; }
</style>
<div id="tip"></div>
<div id="stale" hidden>The server restarted since this page was loaded &mdash; it is running
  older code than the server has. <a href="" onclick="location.reload();return false">Reload</a>.</div>
<div id="pickwrap">
  <div id="picker">
    <header><input type="text" id="pickpath"><button class="pick" id="pickup"
      title="up">&uarr;</button><button class="pick" id="pickclose" title="close">&times;</button></header>
    <div id="picklist"></div>
    <div id="pickroots"></div>
  </div>
</div>
<div id="side">
  <h1>ROI encoder bench <a class="linkish" href="?reset" style="float:right;margin:0"
    title="forget the stored settings and start from the defaults">reset</a></h1>

  <h2>Source</h2>
  <label><span>fixture<i class="q" data-tip="The clips in the tree, each already paired with
    the ROI document that belongs to it -- a scene's boxes only mean anything over that
    scene's picture. They are generated, not committed: run make-synth.sh and
    benchmarks/synthetic/make-scenes.py if the list is short.">?</i></span>
    <select id="fixture"><option value="">(custom path)</option></select></label>
  <label><span>clip<i class="q" data-tip="Any path readable by this machine, relative to the
    repository root or absolute. A browser's own file picker cannot be used here: it hands over
    the file's contents, never its path, and the encoder needs the path -- so the &hellip;
    button browses this machine from the server side instead.">?</i></span>
    <input type="text" id="source" value="testdata/synth.mp4">
    <button class="pick" data-target="source" title="browse">&hellip;</button></label>
  <div id="srcinfo">&mdash;</div>
  <button id="match" class="wide" disabled>Match source settings<i class="q" data-tip="Copy
    the codec, crf and keyint the clip was encoded with, read out of its own bitstream. The
    codec comes along because crf is not one scale between encoders: crf 16 from an x264
    source applied to an x265 run asks for much higher quality than the source has and the
    file comes out half again as large.">?</i></button>
  <p class="matchnote" id="matchnote"></p>

  <h2>Run</h2>
  <label><span>from, s<i class="q" data-tip="Where the segment starts. Stream copy cannot begin
    anywhere -- it snaps back to the keyframe at or before this second -- so the panel above
    reports the second the cut really begins at, and the boxes are shifted by that one. The
    clip's subject need not be in its first two seconds, which is the whole reason this
    exists.">?</i></span><input type="range" id="from" min="0" max="0" step="0.5" value="0">
    <output class="val" id="from_v">0</output></label>
  <label><span>segment, s<i class="q" data-tip="How much of the clip to encode, starting at
    'from'. Cut by stream copy and used as both the encoder input and the reference, so a
    shorter run does not tilt the comparison -- it only makes a knob change cost seconds
    instead of a minute. Every frame of both sides is kept as a PNG, so a long segment is
    gigabytes: on load this is set to the whole clip up to a cap.">?</i></span>
    <input type="range" id="seconds" min="0.5" max="12"
    step="0.5" value="2"><output class="val" id="seconds_v">2</output></label>
  <label><span>flat baseline<i class="q" data-tip="Encode a second time with the ROI
    switched off -- same crf, same gop, no boost and no background suppression -- and
    report both. Input against output compares the source's CRF with ours and says nothing
    about ROI; this is the question ROI exists to answer. Doubles the run.">?</i></span>
    <input type="checkbox" id="flat_baseline"></label>
  <label><span>metrics<i class="q" data-tip="SSIM and PSNR over the zone and over a
    background corner, across the whole segment. Two extra ffmpeg passes; turn it off when
    you only want to look.">?</i></span><input type="checkbox" id="metrics" checked></label>
  <label><span>burn frame numbers<i class="q" data-tip="Stamps the frame number into the
    source before encoding, losslessly, so it travels through the codec. Both panes showing
    the same number is what proves they are the same picture rather than a promise that
    they are. The fixtures are left alone: check.sh measures the background in the corner
    the number would land in, and the clip is built so the background does not
    crawl.">?</i></span><input type="checkbox" id="number_frames"></label>
  <label><span>auto re-encode<i class="q" data-tip="Re-run the encode by itself, 600 ms
    after a knob stops moving, so a knob change shows its result without pressing the
    button. The pause is the point: a slider fires on every pixel of travel, and an encode
    per pixel would queue for minutes. This is not playback -- for that, use Play under the
    panes.">?</i></span><input type="checkbox" id="live"></label>

  <h2>Encoding</h2>
  <label><span>backend<i class="q" data-tip="x264direct and x265direct drive libx264 and
    libx265 through their own APIs (§5.4). x264/x265 are the same codecs through libavcodec,
    kept as the reference the direct paths are compared against.">?</i></span>
    <select id="encoder">
    <option>cpu</option><option>x264</option><option>x264direct</option>
    <option>x265</option><option selected>x265direct</option><option>qsv</option>
    <option>vaapi</option><option>auto</option></select></label>
  <label><span>codec</span><select id="codec">
    <option>h264</option><option selected>h265</option></select></label>
  <label><span>crf</span><input type="range" id="crf" min="10" max="45" value="32">
    <output class="val" id="crf_v">32</output></label>
  <label><span>gop<i class="q" data-tip="Keyframe interval in frames. The source's own is measured and
    shown under Source above; no container field carries it. It dominates the output size
    on a near-static clip -- at crf 32 with no ROI at all, synth.mp4 comes out at 162% of
    its input at GOP 15 and 86% at GOP 50, purely because of how many keyframes there
    are.">?</i></span>
    <input type="range" id="gop" min="1" max="120" value="15">
    <output class="val" id="gop_v">15</output></label>
  <label><span>target bps</span><input type="number" id="target_bitrate" value="0"
    step="100000" min="0"></label>

  <h2>ROI source</h2>
  <label><span>roi/boxes json<i class="q" data-tip="Load a real track, so the box moves
    with the object and the overlay follows the frame slider. Either sidecar shape is
    accepted: samples[] as roi_json.cpp reads it, or frames[] as boxes-to-ass.py writes it.
    Empty means the single synthetic box below.">?</i></span><input type="text" id="roi_file"
    value="" placeholder="(synthetic box)">
    <button class="pick" data-target="roi_file" title="browse">&hellip;</button></label>
  <div id="roiinfo"></div>
  <a id="ass" class="linkish" href="#" hidden>Save .ass<i class="q" data-tip="The same boxes as
    subtitles, for a player: mpv out.mp4 --sub-file=overlay.ass. libass composites them over the
    video without touching a pixel of it, which is what keeps the file comparable to its source
    -- boxes burned into the picture make every metric computed against it wrong.">?</i></a>
  <label><span>override quality<i class="q" data-tip="Keep the loaded file's geometry but
    apply the qp knobs below on top. Off means the document is used exactly as it is on
    disk.">?</i></span><input type="checkbox" id="override_quality" checked></label>

  <h2>Detector<i class="q" data-tip="Runs video_a's detector over the segment and writes the
    boxes sidecar, which then lands in the field above. No encoder and no service is involved:
    the pass decodes, detects and writes JSON. video_a is a sibling checkout, so this is greyed
    out when it is not there.">?</i></h2>
  <div id="detinfo">&mdash;</div>
  <div class="classes" id="classes"></div>
  <label><span>detect fps<i class="q" data-tip="Inference passes per second of content, measured
    against the clip's own timeline so a run is reproducible whatever the machine's speed.
    Frames between passes carry the previous boxes forward. 0.5 is enough for a car crossing a
    frame; raise it when the object is quick.">?</i></span>
    <input type="range" id="detect_fps" min="0.5" max="10" step="0.5" value="2">
    <output class="val" id="detect_fps_v">2</output></label>
  <label><span>min confidence</span>
    <input type="range" id="min_confidence" min="0.1" max="0.9" step="0.05" value="0.5">
    <output class="val" id="min_confidence_v">0.5</output></label>
  <button id="detect" class="wide">Run detector</button>
  <div id="detstatus"></div>

  <h2>ROI quality<i class="q" data-tip="With a document loaded these are its kinds, one row
    each, and the 'kind' select below applies to the synthetic box only. Boxes may sit inside
    one another -- a plate inside a vehicle, a face inside a person -- and the strongest boost
    wins where they overlap.">?</i></h2>
  <label><span>kind<i class="q" data-tip="Applies to the synthetic box only. With a document
    loaded, the kinds are whatever is in it -- a row per kind appears below. The names are the
    ones §11's ladder knows (importance.cpp): license_plate 1.00, face 0.95, person 0.80,
    vehicle 0.60, animal 0.60. Anything else is an unknown class.">?</i></span><select id="kind">
    <option>license_plate</option><option>plate</option><option>face</option>
    <option>person</option><option>vehicle</option><option>animal</option></select></label>
  <label><span>qp_delta<i class="q" data-tip="How much better the object gets than the frame's
    own quantiser. Applies to every kind that has no row of its own below.">?</i></span>
    <input type="range" id="qp_delta" min="-20" max="0" value="-8">
    <output class="val" id="qp_delta_v">-8</output></label>
  <div id="kindrows"></div>
  <p class="matchnote">Overlaps keep the strongest boost: the regions go to the encoder sorted
    by delta and the first one wins where they cross, so a plate inside a vehicle is encoded at
    the plate's delta and the rest of the car at the vehicle's.<i class="q" data-tip="Both
    paths do this -- the rectangle list is sorted ascending by qp_delta (qp_mapper.cpp) and the
    block map is painted last-to-first so index 0 survives (importance_map.cpp). With
    'override quality' off, the deltas come from §11's ladder instead: license_plate 1.00,
    face 0.95, person 0.80, vehicle 0.60.">?</i></p>
  <label><span>background +qp</span><input type="range" id="background_qp_delta" min="0"
    max="30" value="22"><output class="val" id="background_qp_delta_v">22</output></label>
  <label><span>pad_norm<i class="q" data-tip="The halo around each box, as a fraction of
    the frame on each axis. It carries half the boost, and the sweep recorded in STATUS.md
    found it buys about ten times what raising the boost itself does.">?</i></span>
    <input type="range" id="pad_norm" min="0" max="0.2"
    step="0.005" value="0.02"><output class="val" id="pad_norm_v">0.02</output></label>
  <label><span>hold_sec<i class="q" data-tip="How long one submitted sample stands before
    the next replaces it. Applies to the synthetic box only: a loaded track keeps its own
    timing, because overriding it would make every past position of a moving box stay
    protected.">?</i></span><input type="range" id="hold_sec" min="0.1" max="20"
    step="0.1" value="20"><output class="val" id="hold_sec_v">20</output></label>

  <h2>Synthetic box (frame fractions)</h2>
  <label><span>x</span><input type="range" id="bbox_x" min="0" max="0.9" step="0.01"
    value="0.35"><output class="val" id="bbox_x_v">0.35</output></label>
  <label><span>y</span><input type="range" id="bbox_y" min="0" max="0.9" step="0.01"
    value="0.40"><output class="val" id="bbox_y_v">0.40</output></label>
  <label><span>w</span><input type="range" id="bbox_w" min="0.02" max="1" step="0.01"
    value="0.30"><output class="val" id="bbox_w_v">0.30</output></label>
  <label><span>h</span><input type="range" id="bbox_h" min="0.02" max="1" step="0.01"
    value="0.20"><output class="val" id="bbox_h_v">0.20</output></label>

  <button id="go">Encode</button>
</div>

<div id="main">
  <div id="toolbar">
    <label class="inline"><span>zoom<i class="q" data-tip="A 1280-wide frame squeezed into
      a 600-wide pane is downscaled, and downscaling removes exactly the ringing and
      blocking worth looking at. At 100% the pixels are the encoder's own. Panes scroll
      together.">?</i></span><select id="zoom">
      <option value="fit">fit</option><option value="1">100%</option>
      <option value="2">200%</option><option value="4">400%</option></select></label>
    <label class="inline"><span>right pane<i class="q" data-tip="Difference subtracts the
      two frames and multiplies. Loss at a sane CRF is a few levels per pixel: real,
      measurable and invisible next to the picture it sits on. The background lights up and
      a protected zone stays dark.">?</i></span><select id="view">
      <option value="out">ROI encoded</option>
      <option value="diff">difference &times;N</option></select></label>
    <label class="inline" id="gainwrap" hidden><span>&times;</span>
      <input type="range" id="gain" min="1" max="30" value="8" style="width:90px">
      <output class="val" id="gain_v">8</output></label>
  </div>
  <div id="views">
    <div class="pane"><h3>original <em id="lsize"></em></h3><div class="shot" id="lshot">
      <div class="frame" id="lframe"><img id="left">
        <svg viewBox="0 0 1 1" preserveAspectRatio="none"><g id="lbox"></g></svg></div></div></div>
    <div class="pane"><h3 id="rtitle">ROI encoded <em id="rsize"></em></h3>
      <div class="shot" id="rshot">
      <div class="frame" id="rframe"><img id="right">
        <svg viewBox="0 0 1 1" preserveAspectRatio="none"><g id="rbox"></g></svg></div></div></div>
  </div>
  <div id="bar">
    <button id="play" class="mini" disabled>&#9654;</button>
    <span>frame</span>
    <input type="range" id="frame" min="0" max="0" value="0" disabled>
    <output class="val" id="frame_v">0</output>
    <label class="inline"><span>speed<i class="q" data-tip="Playback rate as a fraction of
      the clip's own. The frames are the decoded ones, played in order -- no second encode,
      so nothing is added to what you are judging. Slow it down to watch the background
      pump between keyframes.">?</i></span><select id="speed">
      <option value="0.25">&frac14;&times;</option><option value="0.5">&frac12;&times;</option>
      <option value="1" selected>1&times;</option></select></label>
    <label class="inline"><span>loop</span><input type="checkbox" id="loop" checked></label>
    <span id="prog"><span class="spin"></span><span id="stage"></span></span>
  </div>
  <div class="legend">
    <span><i style="border-color:#39d353"></i>submitted box</span>
    <span><i style="border-color:#f5a623;border-top-style:dashed"></i>halo (pad_norm, half the boost)</span>
  </div>
  <div id="stats"></div>
  <div id="err"></div>
  <div id="log"></div>
</div>

<script>
const $ = id => document.getElementById(id);
const KNOBS = ["crf","gop","qp_delta","background_qp_delta","pad_norm","hold_sec",
               "bbox_x","bbox_y","bbox_w","bbox_h","seconds","from"];
// The whole clip on load, up to this. Every frame of both panes is written out as a PNG, so a
// segment is bytes on disk as well as seconds of encoding: 30 s of 1440x1440 is some three
// gigabytes. Long clips get this much and say so; short ones get all of themselves, which is
// what "the whole clip" means for every fixture in the tree.
const kAutoSegmentSeconds = 10;
const ALL = KNOBS.concat(["encoder","codec","kind","target_bitrate","metrics","roi_file",
                          "override_quality","number_frames","flat_baseline"]);
let runId = null, poll = null, pending = null, detPending = null, srcInfo = null,
    current = null;

// The detector's knobs are synced the same way but stay out of KNOBS and ALL: they re-run
// inference, not the encode, so live mode must not fire on them.
for (const k of KNOBS.concat(["detect_fps", "min_confidence"])) {
  const el = $(k), out = $(k + "_v");
  const sync = () => out.value = el.value;
  el.addEventListener("input", sync); sync();
}

// The boxes, and the halo pad_norm puts around each. Drawn in the same normalized units the
// ROI document uses, over both panes, so what was asked for sits next to what came out.
//
// These are the boxes as submitted. The library still grows each by the halo, lifts it to
// min_roi_side and smooths it across frames (§11, §13); redrawing that arithmetic here
// would show something plausible that is not what the encoder used.
//
// The halo is not decoration -- the padding sweep in STATUS.md found it buys about ten
// times what the boost itself does, and it carries half the boost (pad_qp_delta = qp/2).
function boxesForFrame(index) {
  const p = +$("pad_norm").value;
  if (current && current.track && current.track.length) {
    const fps = current.fps || (srcInfo ? srcInfo.fps : 25) || 25;
    const t = index / fps, hold = current.hold_sec || 0.25, half = 0.5 / fps;
    // The latest sample at or before this frame, and only the samples sharing its timestamp --
    // not everything still inside the hold window. The detector writes a row per frame, and
    // filtering by the window alone drew every past position of the box as a trail.
    let at = null;
    for (const b of current.track)
      if (b.t <= t + half && (at === null || b.t > at)) at = b.t;
    if (at === null || t - at >= hold + half) return [];
    return current.track.filter(b => Math.abs(b.t - at) < 1e-6)
                        .map(b => ({x: b.x, y: b.y, w: b.w, h: b.h, p,
                                    kind: b.kind, confidence: b.confidence}));
  }
  return [{x: +$("bbox_x").value, y: +$("bbox_y").value,
           w: +$("bbox_w").value, h: +$("bbox_h").value, p}];
}

function drawBox(index) {
  const svg = boxesForFrame(index).map(b => {
    const hx = Math.max(0, b.x - b.p), hy = Math.max(0, b.y - b.p);
    const hw = Math.min(1 - hx, b.w + 2 * b.p), hh = Math.min(1 - hy, b.h + 2 * b.p);
    // The label is not decoration either. A detector that reports a road sign as a number
    // plate at 0.73 draws a perfectly good box around the wrong thing, and without the class
    // and the score on the picture that reads as an encoder protecting nothing useful.
    // Counter-scaled in x: the overlay's viewBox is 0..1 on both axes over a frame that is
    // not square, so a glyph drawn straight into it comes out stretched by the aspect ratio.
    const aspect = current && current.width && current.height
      ? current.height / current.width : 1;
    const label = b.kind
      ? `<g transform="translate(${b.x} ${Math.max(0.024, b.y - 0.008)}) scale(${aspect} 1)">` +
        `<text fill="#39d353" font-size="0.03" font-family="monospace">${b.kind}` +
        `${b.confidence ? " " + b.confidence.toFixed(2) : ""}</text></g>`
      : "";
    return `<rect x="${hx}" y="${hy}" width="${hw}" height="${hh}" fill="none"
              stroke="#f5a623" stroke-width="0.003" stroke-dasharray="0.012 0.008"/>` +
           `<rect x="${b.x}" y="${b.y}" width="${b.w}" height="${b.h}" fill="none"
              stroke="#39d353" stroke-width="0.004"/>` + label;
  }).join("");
  $("lbox").innerHTML = svg; $("rbox").innerHTML = svg;
}
for (const k of ["bbox_x","bbox_y","bbox_w","bbox_h","pad_norm"])
  $(k).addEventListener("input", () => drawBox(Number($("frame").value) || 0));
drawBox(0);

// Every knob, kept across reloads. Twenty controls set by hand and lost on refresh is the
// thing that stops a bench being used, and this is the whole of the machinery: the browser's
// own storage, this origin only, no state on the server and nothing written into the tree.
// Saved values are a convenience and never a requirement -- a private window, cleared site
// data or a browser that refuses storage all end up at the defaults, which is why every read
// and write here is wrapped and every failure is silent.
const kStoreKey = "sve-bench-settings-v1";
const STORED = ["source", "roi_file", "seconds", "from", "encoder", "codec", "crf", "gop",
  "target_bitrate", "metrics", "flat_baseline", "number_frames", "live", "override_quality",
  "kind", "qp_delta", "background_qp_delta", "pad_norm", "hold_sec",
  "bbox_x", "bbox_y", "bbox_w", "bbox_h", "detect_fps", "min_confidence",
  "zoom", "view", "gain", "speed", "loop"];

function saveSettings() {
  try {
    const data = {kindQp: kindQp,
                  classes: [...document.querySelectorAll(".cls")].map(c => [c.value, c.checked])};
    for (const id of STORED) {
      const el = $(id);
      if (el) data[id] = el.type === "checkbox" ? el.checked : el.value;
    }
    localStorage.setItem(kStoreKey, JSON.stringify(data));
  } catch (e) { /* storage unavailable; the page works, it just forgets */ }
}

// Returns what was restored, or null. The class checkboxes are not here: they do not exist
// until /detector has answered, so loadDetector applies them itself.
function restoreSettings() {
  let data = null;
  try {
    if (location.search.includes("reset")) localStorage.removeItem(kStoreKey);
    data = JSON.parse(localStorage.getItem(kStoreKey) || "null");
  } catch (e) { return null; }
  if (!data) return null;
  for (const id of STORED) {
    const el = $(id);
    if (!el || data[id] === undefined) continue;
    if (el.type === "checkbox") el.checked = !!data[id];
    else el.value = data[id];
  }
  for (const k of KNOBS.concat(["detect_fps", "min_confidence", "gain"]))
    if ($(k + "_v")) $(k + "_v").value = $(k).value;
  if (data.kindQp) kindQp = data.kindQp;
  return data;
}

// Anything typed, dragged or ticked. Values set from script do not raise these, so those
// places call saveSettings themselves.
let saveTimer = null;
for (const type of ["input", "change"])
  document.addEventListener(type, () => {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(saveSettings, 300);
  }, true);

// One tooltip element, positioned per hover. Text is normalized because the markup wraps
// these strings across lines to stay readable in the source.
const tip = $("tip");
document.addEventListener("mouseover", e => {
  const q = e.target.closest("[data-tip]");
  if (!q) return;
  tip.textContent = q.dataset.tip.replace(/\s+/g, " ").trim();
  tip.style.display = "block";
  const r = q.getBoundingClientRect();
  const box = tip.getBoundingClientRect();
  let x = r.right + 10, y = r.top - 4;
  if (x + box.width > innerWidth - 8) x = Math.max(8, r.left - box.width - 10);
  if (y + box.height > innerHeight - 8) y = Math.max(8, innerHeight - box.height - 8);
  tip.style.left = x + "px";
  tip.style.top = y + "px";
});
document.addEventListener("mouseout", e => {
  if (e.target.closest("[data-tip]")) tip.style.display = "none";
});

function params() {
  const p = {source: $("source").value, encoder: $("encoder").value, codec: $("codec").value,
             kind: $("kind").value, target_bitrate: Number($("target_bitrate").value),
             metrics: $("metrics").checked, roi_file: $("roi_file").value,
             from: Number($("from").value), kind_qp: kindQp,
             number_frames: $("number_frames").checked,
             flat_baseline: $("flat_baseline").checked,
             override_quality: $("override_quality").checked, t: 0.0};
  for (const k of KNOBS) p[k] = Number($(k).value);
  return p;
}
const fmtBytes = n => n > 1e6 ? (n/1e6).toFixed(2)+" MB" : n > 1e3 ? (n/1e3).toFixed(1)+" kB" : n+" B";

async function loadSource(reset = true) {
  try {
    const r = await fetch("/source?path=" + encodeURIComponent($("source").value));
    const i = await r.json();
    if (i.error) { $("srcinfo").textContent = i.error; srcInfo = null; return; }
    srcInfo = i;
    $("srcinfo").innerHTML =
      `<b>${i.codec}</b> ${i.width}&times;${i.height}, <b>${i.fps_text}</b> fps<br>` +
      `${i.frames} frames, ${i.duration.toFixed(2)} s, ${fmtBytes(i.bytes)}<br>` +
      `${i.keyframes} keyframes &rarr; GOP &asymp; <b>${i.gop}</b>`;
    // The window follows the clip: the whole of it when that is affordable, its head when it
    // is not. Two seconds of a 96-second clip was the default before, and on a clip whose
    // subject arrives at t=44 that answers "nothing here" without saying why.
    const whole = Math.max(1, Math.ceil(i.duration));
    $("seconds").max = whole;
    $("from").max = Math.max(0, Math.floor(i.duration - 0.5));
    // Only when the clip changed. On a reload the window is whatever was restored, and
    // resetting it here would undo the restore on the first fetch of the page.
    if (reset) {
      $("seconds").value = Math.min(whole, kAutoSegmentSeconds);
      $("seconds_v").value = $("seconds").value;
      $("from").value = 0; $("from_v").value = 0;
      saveSettings();
    }
    for (const el of [$("lframe"), $("rframe")])
      el.style.aspectRatio = i.width + " / " + i.height;
    const set = i.settings || {};
    // crf can only come from the bitstream, and most cameras write none. The codec and the
    // keyframe interval do not need it -- the codec is in the container and the GOP was
    // measured above -- and refusing the whole button for the sake of the one missing field
    // is what left a phone clip with no way to copy either.
    const gop = set.gop || i.gop;
    $("match").disabled = !(set.crf || gop || i.codec);
    $("matchnote").textContent = set.crf || set.gop
      ? `read from the bitstream: ${set.encoder || "?"}` +
        (set.crf ? `, crf ${set.crf}` : "") + (set.gop ? `, keyint ${set.gop}` : "")
      : `no encoder settings in the bitstream; codec and measured GOP ${i.gop} can still be copied`;
  } catch (e) { $("srcinfo").textContent = String(e); }
}

// "Encode it the way it already was", so input against output is one comparison rather
// than two settings arguing. crf is not a container field and cannot be probed -- but x264
// and x265 both write their whole option string into a user-data SEI, and it survives
// muxing, so for anything either produced this copies a fact rather than a guess.
//
// The codec has to come along, and the first version leaving it alone was a trap. CRF is
// not one scale: carrying crf 16 off an x264 source onto an x265 run asks for far higher
// quality than the source has, and the output comes out half again as large. Measured on
// synth.mp4, same crf 16 and gop 50, two second cut against a 56.5 kB input:
//
//     x264direct  ROI off 57.9 kB (102%)   ROI on 46.8 kB (82%)
//     x265direct  ROI off 79.1 kB (140%)   ROI on 69.9 kB (123%)
//
// Same numbers, different meaning. On the source's own codec the ROI run is smaller than
// the source, which is the answer the button exists to make visible.
//
// The backend follows only when it has to: a codec-specific one that cannot encode what
// was just selected would fail the run outright.
$("match").addEventListener("click", () => {
  const set = (srcInfo && srcInfo.settings) || {};
  const applied = [];
  if (srcInfo && srcInfo.codec) {
    const codec = srcInfo.codec === "hevc" || srcInfo.codec === "h265" ? "h265"
                : srcInfo.codec === "h264" ? "h264" : null;
    if (codec) {
      $("codec").value = codec;
      applied.push(codec);
      const backend = $("encoder").value;
      const wrong = (codec === "h265" && backend.startsWith("x264"))
                 || (codec === "h264" && backend.startsWith("x265"));
      if (wrong) {
        $("encoder").value = codec === "h265" ? "x265direct" : "x264direct";
        applied.push("backend " + $("encoder").value);
      }
    }
  }
  // The bitstream's keyint when it has one, the measured interval when it does not -- the
  // number under Source is a count of keyframes, which every clip has.
  const gop = set.gop || (srcInfo && srcInfo.gop);
  if (gop) {
    $("gop").value = Math.min(gop, Number($("gop").max));
    $("gop_v").value = $("gop").value;
    applied.push("gop " + $("gop").value + (set.gop ? "" : " (measured)"));
  }
  if (set.crf) {
    const crf = Math.round(set.crf);
    $("crf").value = crf; $("crf_v").value = crf; applied.push("crf " + crf);
  }
  $("target_bitrate").value = 0;
  $("matchnote").textContent = applied.length ? "applied " + applied.join(", ")
                                              : "nothing to apply";
});
$("source").addEventListener("change", () => loadSource());

async function loadFixtures() {
  const list = await (await fetch("/fixtures")).json();
  const sel = $("fixture");
  for (const f of list) {
    const o = document.createElement("option");
    o.value = JSON.stringify(f); o.textContent = f.label;
    sel.appendChild(o);
  }
  // Preselect whatever the clip field already points at, so the two never disagree.
  for (const o of sel.options)
    if (o.value && JSON.parse(o.value).path === $("source").value) { sel.value = o.value; break; }
}
$("fixture").addEventListener("change", () => {
  if (!$("fixture").value) return;
  const f = JSON.parse($("fixture").value);
  $("source").value = f.path;
  $("roi_file").value = f.roi;
  loadSource(); loadRoi();
});

// Tell the tab when it has fallen behind the server. Cheap, and it removes a whole class of
// confusion: a page kept open across a restart runs the script it was served, so a change
// already on disk reads as a change that did not work.
let buildId = null;
setInterval(async () => {
  try {
    const v = await (await fetch("/version")).json();
    if (buildId === null) buildId = v.build;
    else if (v.build !== buildId) $("stale").hidden = false;
  } catch (e) { /* server gone; nothing useful to say */ }
}, 3000);

async function loadRoi() {
  const ref = $("roi_file").value.trim();
  $("ass").hidden = !ref;
  if (!ref) { $("roiinfo").textContent = ""; showKinds([]); return; }
  const r = await fetch("/roi?path=" + encodeURIComponent(ref));
  const i = await r.json();
  if (i.error) {
    // Almost always a detector sidecar from a previous run of the server: those live in a
    // temporary directory that is removed when it stops, while the field naming one is
    // remembered across reloads. Clearing it is kinder than letting the encode fail on it.
    const gone = i.error.startsWith("no such file");
    $("roiinfo").innerHTML = gone
      ? `<span class="warn">that sidecar is gone — the bench was restarted; run the detector
         again</span>`
      : i.error;
    if (gone) { $("roi_file").value = ""; $("ass").hidden = true; showKinds([]); saveSettings(); }
    return;
  }
  // Against the window, not against the clip. A sidecar is written on the source's own
  // timeline and the encode sees one segment of it, so what matters is whether any box falls
  // inside that segment: a document full of boxes and a window that misses all of them is a
  // run with no ROI at all, and nothing on the page used to say so.
  const from = Number($("from").value), seconds = Number($("seconds").value);
  const inside = i.t_max >= from && i.t_min < from + seconds;
  showKinds(i.kinds);
  $("roiinfo").innerHTML =
    `${i.count} samples, kinds: ${i.kinds.join(", ") || "—"}, t ∈ ` +
    `[${i.t_min.toFixed(2)}, ${i.t_max.toFixed(2)}] s` +
    (inside ? "" : `<br><span class="warn">no box falls in the window ` +
                   `${from.toFixed(1)}–${(from + seconds).toFixed(1)} s</span>`);
}
$("roi_file").addEventListener("change", loadRoi);
for (const k of ["seconds", "from"]) $(k).addEventListener("change", loadRoi);

// The same boxes for anything that is not this page. Width and height ride along because a
// samples[] document carries fractions only and ASS draws in pixels.
$("ass").addEventListener("click", e => {
  e.preventDefault();
  const ref = $("roi_file").value.trim();
  if (!ref) return;
  const w = srcInfo ? srcInfo.width : 0, h = srcInfo ? srcInfo.height : 0;
  location.href = `/ass?path=${encodeURIComponent(ref)}&w=${w}&h=${h}`;
});

// A slider per kind the document actually contains. Rebuilt whenever the document changes,
// keeping any value already set for a kind that survives -- retyping the plate's delta because
// a re-detection found one more class is the kind of thing that makes a bench unused.
// The starting values are §11's ladder scaled onto the shared slider: a plate gets all of it,
// a vehicle rather less, so the default is the specification's ordering rather than "the same
// for everything".
const kLadder = {license_plate: 1.0, plate: 1.0, face: 0.95, person: 0.8, vehicle: 0.6,
                 animal: 0.6};
let kindQp = {};
function showKinds(kinds) {
  const base = Number($("qp_delta").value);
  const rows = (kinds || []).map(k => {
    if (kindQp[k] === undefined) kindQp[k] = Math.round(base * (kLadder[k] === undefined ? 1 : kLadder[k]));
    return `<label><span>${k}</span><input type="range" class="kq" data-kind="${k}" min="-20"
      max="0" value="${kindQp[k]}"><output class="val">${kindQp[k]}</output></label>`;
  }).join("");
  $("kindrows").innerHTML = rows;
  for (const el of document.querySelectorAll(".kq"))
    el.addEventListener("input", () => {
      kindQp[el.dataset.kind] = Number(el.value);
      el.nextElementSibling.value = el.value;
      if ($("live").checked) { clearTimeout(timer); timer = setTimeout(start, 600); }
    });
}

function showStats(i) {
  const num = v => v === null || v === undefined ? "—" : Number(v).toFixed(6);
  const ratio = i.clip_bytes ? (100 * i.bytes / i.clip_bytes).toFixed(1) + "%" : "—";
  const rows = [
    ["input segment", fmtBytes(i.clip_bytes) + " · " + i.clip_frames + " frames"],
    ["output", fmtBytes(i.bytes)],
    ["vs input", ratio + " — but the input has its own crf and GOP; compare with flat"],
    ["frames", i.frames + " of " + i.clip_frames],
    // What the SSIM below was measured over. It is the rectangle containing every box of the
    // run, and with a detector's several objects that can be most of the picture -- at which
    // point "zone" and "background" stop being two different things and the numbers below
    // stop separating them. Shown as a share of the frame so that is visible rather than
    // inferred.
    ["zone", i.zone[2] + "×" + i.zone[3] + " · " +
             (100 * i.zone[2] * i.zone[3] / (i.width * i.height)).toFixed(0) + "% of frame"],
    ["format", i.codec + " " + i.fps_text + " fps, " + i.duration.toFixed(2) + " s"],
    ["SSIM zone", num(i.ssim_zone)],
    ["SSIM background", num(i.ssim_bg)],
    ["PSNR zone", i.psnr_zone == null ? "—" : Number(i.psnr_zone).toFixed(2) + " dB"],
    ["run time", i.seconds == null ? "—" : i.seconds + " s"],
  ];
  // The flat encode is the only comparison that isolates what ROI did: same knobs, same
  // codec, ROI off. Shown beside the ROI numbers rather than instead of them.
  if (i.flat_bytes != null) {
    const d = 100 * (i.bytes - i.flat_bytes) / i.flat_bytes;
    rows.push(["flat (no ROI)", fmtBytes(i.flat_bytes)]);
    rows.push(["ROI vs flat", (d >= 0 ? "+" : "") + d.toFixed(1) + "% size"]);
    if (i.flat_ssim_zone != null)
      rows.push(["zone: ROI vs flat", num(i.ssim_zone) + " vs " + num(i.flat_ssim_zone)]);
    if (i.flat_ssim_bg != null)
      rows.push(["bg: ROI vs flat", num(i.ssim_bg) + " vs " + num(i.flat_ssim_bg)]);
  }
  $("stats").innerHTML = rows.map(([k, v]) => "<div><b>" + k + "</b>" + v + "</div>").join("");
}

function loadFrame(n) {
  if (!runId) return;
  $("frame_v").value = n;
  const view = $("view").value, gain = $("gain").value;
  $("left").src  = "/img/" + runId + "/src/" + n + ".png";
  $("right").src = view === "diff"
    ? "/img/" + runId + "/diff/" + n + ".png?gain=" + gain
    : "/img/" + runId + "/out/" + n + ".png";
  // The box means nothing over a difference map; the difference is the subject there.
  $("rbox").style.display = view === "diff" ? "none" : "";
  drawBox(n);
}
$("frame").addEventListener("input", e => { stop(); loadFrame(Number(e.target.value)); });

// Playback, which is what most people mean by watching a codec work. Some of what ROI
// encoding does is only visible in motion -- the background pumping between keyframes, and
// whether §13's anti-flicker is holding the zone steady -- and a still frame cannot show
// either. These are the decoded frames played in order, not a second encode, so nothing is
// added to the thing being judged.
let timerId = null;
function playing() { return timerId !== null; }
function stop() {
  if (timerId) clearInterval(timerId);
  timerId = null;
  $("play").innerHTML = "&#9654;";
}
function play() {
  if (!runId || playing()) return;
  const fps = (current && current.fps ? current.fps : 25) * Number($("speed").value);
  $("play").innerHTML = "&#10073;&#10073;";
  timerId = setInterval(() => {
    const last = Number($("frame").max), next = Number($("frame").value) + 1;
    if (next > last) {
      if (!$("loop").checked) { stop(); return; }
      $("frame").value = 0;
    } else {
      $("frame").value = next;
    }
    loadFrame(Number($("frame").value));
  }, Math.max(20, 1000 / Math.max(1, fps)));
}
$("play").addEventListener("click", () => playing() ? stop() : play());
$("speed").addEventListener("change", () => { if (playing()) { stop(); play(); } });

// Zoom, because this is where "I see no difference" usually comes from: a 1280-wide frame
// squeezed into a 600-wide pane is downscaled, and downscaling is a low-pass filter that
// removes exactly the ringing and blocking worth looking at. At 100% the pixels are the
// encoder's own. The panes scroll together so the two views never drift apart.
function applyZoom() {
  const z = $("zoom").value;
  const fit = z === "fit";
  for (const [shot, frame] of [["lshot","lframe"], ["rshot","rframe"]]) {
    $(shot).classList.toggle("zoomed", !fit);
    $(frame).style.width = fit || !srcInfo ? "" : (srcInfo.width * Number(z)) + "px";
    $(frame).style.height = fit || !srcInfo ? "" : (srcInfo.height * Number(z)) + "px";
  }
}
$("zoom").addEventListener("change", applyZoom);
let syncing = false;
for (const [a, b] of [["lshot","rshot"], ["rshot","lshot"]])
  $(a).addEventListener("scroll", () => {
    if (syncing) return;
    syncing = true;
    $(b).scrollTop = $(a).scrollTop; $(b).scrollLeft = $(a).scrollLeft;
    syncing = false;
  });

$("view").addEventListener("change", () => {
  const diff = $("view").value === "diff";
  $("gainwrap").hidden = !diff;
  $("rtitle").innerHTML = (diff ? "difference" : "ROI encoded") + ' <em id="rsize"></em>';
  if (current) showSizes(current);
  loadFrame(Number($("frame").value) || 0);
});
$("gain").addEventListener("input", () => {
  $("gain_v").value = $("gain").value;
  if ($("view").value === "diff") loadFrame(Number($("frame").value) || 0);
});

function showSizes(i) {
  const l = $("lsize"), r = $("rsize");
  if (l) l.textContent = `${i.width}×${i.height} · ${fmtBytes(i.clip_bytes)}`;
  // Bytes, and a percentage only against the flat baseline. The percentage against the
  // input used to sit here and it measured the wrong thing: the input has its own crf and
  // its own GOP, so on synth.mp4 -- crf 16, GOP 50 -- encoding at the bench default GOP 15
  // reads 162% with the ROI switched off entirely. That number is the GOP difference, and
  // reading it as "ROI made the file bigger" is the mistake it invites.
  if (r) r.textContent = $("view").value === "diff" ? ""
    : `${i.width}×${i.height} · ${fmtBytes(i.bytes)}` +
      (i.flat_bytes ? ` · ${(100 * (i.bytes - i.flat_bytes) / i.flat_bytes).toFixed(0)}% vs flat` : "");
}

function busy(on, text) {
  $("prog").classList.toggle("on", on);
  $("stage").textContent = text || "";
  $("go").disabled = on;
}

// One poller for both jobs. The encode and the detector report through the same /job
// endpoint, and a second copy of this loop would drift from it the moment the protocol grows
// a field. `stale` is what lets a newer run take over from an older one.
function follow(job, {stale, stage, done}) {
  const timer = setInterval(async () => {
    const s = await (await fetch("/job?id=" + job)).json();
    if (stale && stale()) { clearInterval(timer); return; }
    stage(s.stage, !s.done);
    if (!s.done) return;
    clearInterval(timer);
    done(s);
  }, 250);
  return timer;
}

async function start() {
  stop();
  $("err").textContent = "";
  busy(true, "queued");
  const r = await fetch("/run", {method: "POST", body: JSON.stringify(params())});
  const {job, error} = await r.json();
  if (error) { busy(false, ""); $("err").textContent = error; return; }
  pending = job;
  if (poll) clearInterval(poll);
  poll = follow(job, {
    stale: () => job !== pending,
    stage: (text, running) => busy(running, text),
    done: s => {
    poll = null; busy(false, "");
    if (s.error) { $("err").textContent = s.error; return; }
    const i = s.result;
    runId = i.id; current = i; showStats(i); showSizes(i); applyZoom();
    $("log").textContent = i.log || "";
    const last = Math.max(0, i.frames - 1);
    const keep = Math.min(Number($("frame").value) || 0, last);
    $("frame").max = last; $("frame").disabled = false; $("frame").value = keep;
    $("play").disabled = last === 0;
    loadFrame(keep);
  }});
}
$("go").addEventListener("click", start);

// The detector, which is a separate button rather than a stage of the encode on purpose:
// inference costs seconds per second of content and depends on none of the encoder's knobs,
// so tying it to Encode would re-run it on every change of crf. The result is cached by clip
// and detector settings, so pressing this twice costs nothing.
async function loadDetector() {
  try {
    const d = await (await fetch("/detector")).json();
    $("classes").innerHTML = d.classes.map(c =>
      `<label><input type="checkbox" class="cls" value="${c}" checked><span>${c}</span></label>`
    ).join("");
    // Applied here rather than in restoreSettings: these boxes do not exist until /detector
    // has answered, and there is nothing to tick before that.
    for (const [name, on] of (restored && restored.classes) || [])
      for (const el of document.querySelectorAll(".cls"))
        if (el.value === name) el.checked = on;
    $("detect").disabled = !d.ok;
    $("detinfo").innerHTML = d.ok
      ? `worker: ${d.worker}<br>models: ${d.models}`
      : `<span class="warn">${d.missing.join("<br>")}</span>`;
  } catch (e) { $("detinfo").textContent = String(e); }
}

async function detect() {
  $("err").textContent = "";
  $("detect").disabled = true;
  $("detstatus").textContent = "queued";
  const body = {source: $("source").value, seconds: Number($("seconds").value),
                detect_fps: Number($("detect_fps").value),
                min_confidence: Number($("min_confidence").value),
                classes: [...document.querySelectorAll(".cls:checked")].map(c => c.value)};
  const r = await fetch("/detect", {method: "POST", body: JSON.stringify(body)});
  const {job, error} = await r.json();
  if (error) { $("detect").disabled = false; $("detstatus").textContent = "";
               $("err").textContent = error; return; }
  detPending = job;
  follow(job, {
    stale: () => job !== detPending,
    stage: text => $("detstatus").textContent = text,
    done: s => {
      $("detect").disabled = false;
      if (s.error) { $("detstatus").textContent = ""; $("err").textContent = s.error; return; }
      const i = s.result;
      $("roi_file").value = i.boxes;
      loadRoi();
      saveSettings();
      // "Nothing found" said out loud, and where they are when there is something. An empty
      // sidecar encodes without any ROI at all, and silence there reads as an encoder that
      // stopped working -- which is what a two-second window on a clip whose first car
      // arrives at t=44 used to look like.
      $("detstatus").innerHTML = i.count
        ? `${i.count} boxes over ${i.frames} frames: ${i.kinds.join(", ")}, ` +
          `t ∈ [${i.t_min.toFixed(1)}, ${i.t_max.toFixed(1)}] s of ${i.duration.toFixed(0)} s` +
          (i.cached ? " (cached)" : ` in ${i.seconds} s`) +
          (i.t_min >= Number($("from").value) + Number($("seconds").value)
            ? ` <a href="#" id="jump">move the window there</a>` : "")
        : `<span class="warn">nothing detected — try a lower min confidence: this material ` +
          `gives none at 0.5 and some at 0.25</span>`;
      const jump = $("jump");
      if (jump) jump.addEventListener("click", e => {
        e.preventDefault();
        // Back off half a second: the cut snaps to the keyframe at or before this, and landing
        // exactly on the first box would put it in the first frame with nothing before it.
        const at = Math.max(0, Math.floor((i.t_min - 0.5) * 2) / 2);
        $("from").value = at; $("from_v").value = at;
        loadRoi();
        saveSettings();
      });
    }});
}
$("detect").addEventListener("click", detect);

// The file picker. Server-side listing, because a browser's own file input hands over the
// file's contents and never its path -- and every path here is given to a subprocess, not to
// the page. Videos and JSON only: those are the two things a field on the left takes.
let pickTarget = null;
const fmtSize = n => n > 1e9 ? (n/1e9).toFixed(1)+" GB" : n > 1e6 ? (n/1e6).toFixed(1)+" MB"
                             : n > 1e3 ? (n/1e3).toFixed(0)+" kB" : n+" B";

async function pickList(dir) {
  const d = await (await fetch("/browse?dir=" + encodeURIComponent(dir || ""))).json();
  $("pickpath").value = d.dir;
  $("pickup").disabled = !d.parent;
  $("pickup").dataset.parent = d.parent || "";
  $("picklist").innerHTML =
    (d.error ? `<div class="warn">${d.error}</div>` : "") +
    d.dirs.map(n => `<div class="dir" data-dir="${encodeURIComponent(n)}">${n}/</div>`).join("") +
    d.files.map(f => `<div data-file="${encodeURIComponent(f.name)}">${f.name}` +
                     `<span>${fmtSize(f.size)}</span></div>`).join("");
  $("pickroots").innerHTML =
    d.roots.map(r => `<button data-root="${encodeURIComponent(r)}">${r}</button>`).join("");
}

function openPicker(target) {
  pickTarget = target;
  $("pickwrap").classList.add("on");
  // Starts where the field already points, so re-picking a neighbour of the current clip is
  // one click rather than a walk down from the root.
  pickList($(target).value.trim());
}
function closePicker() { $("pickwrap").classList.remove("on"); pickTarget = null; }

for (const b of document.querySelectorAll("button.pick[data-target]"))
  b.addEventListener("click", () => openPicker(b.dataset.target));
$("pickclose").addEventListener("click", closePicker);
$("pickup").addEventListener("click", () => pickList($("pickup").dataset.parent));
$("pickwrap").addEventListener("click", e => { if (e.target === $("pickwrap")) closePicker(); });
document.addEventListener("keydown", e => { if (e.key === "Escape") closePicker(); });
$("pickpath").addEventListener("change", () => pickList($("pickpath").value));
$("pickroots").addEventListener("click", e => {
  const b = e.target.closest("[data-root]");
  if (b) pickList(decodeURIComponent(b.dataset.root));
});
$("picklist").addEventListener("click", e => {
  const row = e.target.closest("[data-dir], [data-file]");
  if (!row) return;
  const here = $("pickpath").value.replace(/\/$/, "");
  if (row.dataset.dir !== undefined) {
    pickList(here + "/" + decodeURIComponent(row.dataset.dir));
    return;
  }
  $(pickTarget).value = here + "/" + decodeURIComponent(row.dataset.file);
  if (pickTarget === "source") { $("fixture").value = ""; loadSource(); } else { loadRoi(); }
  saveSettings();
  closePicker();
});

// Live mode: every knob re-runs after a pause. The pause is what makes it usable -- a
// slider fires on every pixel of travel, and an encode per pixel would queue for minutes.
let timer = null;
for (const k of ALL) $(k).addEventListener("input", () => {
  if (!$("live").checked) return;
  clearTimeout(timer);
  timer = setTimeout(start, 600);
});
$("live").addEventListener("change", () => { if ($("live").checked) start(); });

// Startup, in one place and last, because everything above is definitions and the order here
// is what makes a reload look like the tab was never closed: restore the knobs, then ask the
// server about the clip they name -- without letting that ask overwrite the window that was
// just restored.
const restored = restoreSettings();
loadSource(!restored);
loadFixtures();
loadDetector();
if ($("roi_file").value.trim()) loadRoi(); else $("ass").hidden = true;
$("gainwrap").hidden = $("view").value !== "diff";
$("rtitle").innerHTML = ($("view").value === "diff" ? "difference" : "ROI encoded")
                      + ' <em id="rsize"></em>';
applyZoom();
</script>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code: int = 200) -> None:
        self._send(code, json.dumps(obj).encode(), "application/json")

    def do_GET(self) -> None:
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        path = url.path
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            return
        if path == "/source":
            raw = (query.get("path") or [""])[0]
            src = Path(raw)
            if not src.is_absolute():
                src = ROOT / src
            if not src.exists():
                self._json({"error": f"no such file: {src}"})
                return
            # Without key_times: the page needs the count, not the list, and an hour of video
            # is thousands of floats on every reload.
            self._json({k: v for k, v in source_info(src).items() if k != "key_times"})
            return
        if path == "/version":
            self._json({"build": BUILD_ID})
            return
        if path == "/fixtures":
            self._json(fixtures())
            return
        if path == "/detector":
            self._json(detector_status())
            return
        if path == "/browse":
            self._json(browse((query.get("dir") or [""])[0]))
            return
        if path == "/ass":
            raw = (query.get("path") or [""])[0]
            ref = Path(raw)
            if not ref.is_absolute():
                ref = ROOT / ref
            try:
                # The frame size comes from the clip when the document has none: a samples[]
                # document carries geometry in fractions only, and ASS needs pixels to draw in.
                body = ass_of(json.loads(ref.read_text()),
                              int(as_number((query.get("w") or ["0"])[0])),
                              int(as_number((query.get("h") or ["0"])[0])),
                              int(as_number((query.get("thickness") or ["3"])[0], 3)))
            except Exception as exc:
                self._json({"error": str(exc)}, 400)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Disposition",
                             f'attachment; filename="{ref.stem}.ass"')
            data = body.encode()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if path == "/roi":
            raw = (query.get("path") or [""])[0]
            ref = Path(raw)
            if not ref.is_absolute():
                ref = ROOT / ref
            if not ref.exists():
                self._json({"error": f"no such file: {ref}"})
                return
            try:
                raw = json.loads(ref.read_text())
                track = track_of(normalise_roi(raw))
            except Exception as exc:
                self._json({"error": f"cannot read {ref.name}: {exc}"})
                return
            times = [b["t"] for b in track] or [0.0]
            # How far the document describes, which is not where the last box is: a detector
            # writes a row for every frame and finds nothing in some of them, so the last box
            # can sit well before the end of a clip that is covered to its last frame.
            rows = [float(f.get("t", 0.0)) for f in raw.get("frames", [])]
            self._json({"count": len(track), "t_min": min(times), "t_max": max(times),
                        "covered": max(rows) if rows else max(times),
                        "kinds": sorted({b["kind"] for b in track})})
            return
        if path == "/job":
            job_id = (query.get("id") or [""])[0]
            with LOCK:
                job = JOBS.get(job_id)
            self._json(job or {"stage": "?", "done": True, "error": "no such job"})
            return
        parts = path.strip("/").split("/")
        if len(parts) == 4 and parts[0] == "img" and parts[3].endswith(".png"):
            _, run_id, side, name = parts
            with LOCK:
                entry = RUNS.get(run_id)
            if entry is None or side not in ("src", "out", "diff"):
                self._send(404, b"no such run", "text/plain")
                return
            try:
                index = int(name[:-4])
            except ValueError:
                self._send(400, b"bad frame", "text/plain")
                return
            if side == "diff":
                gain = int((query.get("gain") or ["8"])[0])
                dest = difference(entry, index, max(1, min(gain, 40)))
                if dest is None:
                    self._send(404, b"no such frame", "text/plain")
                    return
                self._send(200, dest.read_bytes(), "image/png")
                return
            dest = entry["dir"] / "frames" / f"{side}_{index}.png"
            if not extract(entry[side], index, dest):
                self._send(404, b"no such frame", "text/plain")
                return
            self._send(200, dest.read_bytes(), "image/png")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path not in ("/run", "/detect"):
            self._send(404, b"not found", "text/plain")
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            params = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError as exc:
            self._json({"error": str(exc)}, 400)
            return
        if path == "/run" and not CLI.exists():
            self._json({"error": f"roi-transcode not found at {CLI}"}, 500)
            return
        detect = path == "/detect"
        with LOCK:
            COUNTER[0] += 1
            job_id = f"{'d' if detect else 'r'}{COUNTER[0]:04d}"
            JOBS[job_id] = {"id": job_id, "stage": "queued", "done": False,
                            "error": None, "result": None}
        threading.Thread(target=detect_job if detect else encode_job,
                         args=(job_id, params), daemon=True).start()
        self._json({"job": job_id})


def main() -> int:
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            print(f"{tool} is not on PATH", file=sys.stderr)
            return 1
    if not CLI.exists():
        print(f"roi-transcode not found at {CLI}\n"
              f"build it with SVE_BUILD_INTEGRATION=ON, or set BUILD=", file=sys.stderr)
        return 1
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler) as srv:
        # Flushed explicitly: Python buffers stdout whenever it is not a terminal, and an
        # editor watching for this line to open a browser would wait for the buffer rather
        # than for the server. Correct however it is launched, which a PYTHONUNBUFFERED in
        # one launch configuration is not.
        print(f"bench on http://127.0.0.1:{PORT}   (runs in {WORK})", flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
