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
"""

from __future__ import annotations

import http.server
import json
import os
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

WORK = Path(tempfile.mkdtemp(prefix="sve-bench-"))
RUNS: dict[str, dict] = {}
JOBS: dict[str, dict] = {}
CLIPS: dict[tuple[str, float], Path] = {}
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
        "duration": float(probe(path, "format=duration", stream=False) or 0.0),
        "keyframes": len(keys),
        # Average, not a promise: a clip with scene cuts has no single GOP, and rounding
        # this to one number is what the field is for -- a starting point, not a claim.
        "gop": round(frames / len(keys)) if keys and frames else 0,
        "bytes": path.stat().st_size,
    }
    with LOCK:
        SOURCES[key] = info
    return info


def clip_of(src: Path, seconds: float, total: float) -> Path:
    """A short head of the clip, so a knob change costs seconds rather than a minute.

    Stream-copied from t=0, which is frame-exact because frame 0 is a keyframe, and used as
    both the encoder's input and the reference it is compared against -- so shortening the
    run does not tilt the comparison.
    """
    if seconds <= 0 or seconds >= total:
        return src
    key = (str(src), seconds)
    with LOCK:
        if key in CLIPS and CLIPS[key].exists():
            return CLIPS[key]
    dest = WORK / f"clip_{abs(hash(key)) & 0xFFFFFF:06x}.mp4"
    proc = run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(src),
                "-t", str(seconds), "-c", "copy", str(dest)])
    if proc.returncode != 0 or not dest.exists():
        return src
    with LOCK:
        CLIPS[key] = dest
    return dest


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
            samples.append(row)
    out.pop("frames", None)
    out["samples"] = samples
    return out


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
            "x": float(s.get("bbox_x", s.get("x", 0.0))),
            "y": float(s.get("bbox_y", s.get("y", 0.0))),
            "w": float(s.get("bbox_w", s.get("w", 0.0))),
            "h": float(s.get("bbox_h", s.get("h", 0.0))),
        })
    return rows


def build_roi(p: dict) -> dict:
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
        doc = load_roi(path)
        if p.get("override_quality", True):
            doc["background_qp_delta"] = p["background_qp_delta"]
            kinds = {s["kind"] for s in doc.get("samples", []) if s.get("kind")}
            doc["kinds"] = {k: {"qp_delta": p["qp_delta"], "pad_norm": p["pad_norm"]}
                            for k in (kinds or {p["kind"]})}
            doc["hold_sec"] = p["hold_sec"]
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
        clip = clip_of(src, float(p.get("seconds") or 0), info["duration"])

        run_id = job_id
        rundir = WORK / run_id
        (rundir / "frames").mkdir(parents=True, exist_ok=True)
        roi_path, out_path = rundir / "roi.json", rundir / "out.mp4"
        doc = build_roi(p)
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
        zx, zy = int(p["bbox_x"] * width), int(p["bbox_y"] * height)
        zw = max(16, int(p["bbox_w"] * width) // 2 * 2)
        zh = max(16, int(p["bbox_h"] * height) // 2 * 2)
        zone = (min(zx, width - zw), min(zy, height - zh), zw, zh)
        bg = (16, 16, zw, zh)

        result = {
            "id": run_id, "bytes": out_path.stat().st_size,
            "clip_bytes": clip.stat().st_size,
            "frames": frame_count(out_path), "clip_frames": frame_count(clip),
            "width": width, "height": height,
            "codec": probe(out_path, "stream=codec_name"),
            "fps_text": probe(out_path, "stream=r_frame_rate"),
            "duration": float(probe(out_path, "format=duration", stream=False) or 0.0),
            "zone": zone, "bg": bg, "log": log,
            "track": track_of(doc), "hold_sec": doc.get("hold_sec", 0.25),
            "fps": info["fps"],
            "ssim_zone": None, "ssim_bg": None, "psnr_zone": None,
        }
        with LOCK:
            RUNS[run_id] = {"src": clip, "out": out_path, "dir": rundir}
            JOBS[job_id]["result"] = dict(result)

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
  .hint { font-size:11px; opacity:.55; margin:2px 0 0; }
  .legend { display:flex; gap:14px; font-size:11px; opacity:.7; }
  .legend i { display:inline-block; width:20px; height:0; border-top:2px solid; margin-right:5px;
              vertical-align:middle; }
</style>
<div id="side">
  <h1>ROI encoder bench</h1>

  <h2>Source</h2>
  <label><span>clip</span><input type="text" id="source" value="testdata/synth.mp4"></label>
  <div id="srcinfo">&mdash;</div>

  <h2>Run</h2>
  <label><span>segment, s</span><input type="range" id="seconds" min="0.5" max="12"
    step="0.5" value="2"><output class="val" id="seconds_v">2</output></label>
  <p class="hint">Cut by stream copy from frame 0 and used as both the encoder input and the
     reference, so a shorter run does not tilt the comparison.</p>
  <label><span>metrics</span><input type="checkbox" id="metrics" checked></label>
  <label><span>live</span><input type="checkbox" id="live"></label>

  <h2>Encoding</h2>
  <label><span>backend</span><select id="encoder">
    <option>cpu</option><option>x264</option><option>x264direct</option>
    <option>x265</option><option selected>x265direct</option><option>qsv</option>
    <option>vaapi</option><option>auto</option></select></label>
  <label><span>codec</span><select id="codec">
    <option>h264</option><option selected>h265</option></select></label>
  <label><span>crf</span><input type="range" id="crf" min="10" max="45" value="28">
    <output class="val" id="crf_v">28</output></label>
  <label><span>gop</span><input type="range" id="gop" min="1" max="120" value="15">
    <output class="val" id="gop_v">15</output></label>
  <p class="hint" id="gophint"></p>
  <label><span>target bps</span><input type="number" id="target_bitrate" value="0"
    step="100000" min="0"></label>

  <h2>ROI source</h2>
  <label><span>roi/boxes json</span><input type="text" id="roi_file" value=""
    placeholder="(synthetic box)"></label>
  <p class="hint">Either sidecar shape: samples[] as the CLI reads, or frames[] as
     boxes-to-ass.py writes. Empty means the single box below.</p>
  <div id="roiinfo"></div>
  <label><span>override quality</span><input type="checkbox" id="override_quality" checked></label>

  <h2>ROI quality</h2>
  <label><span>kind</span><select id="kind">
    <option>plate</option><option>face</option><option>person</option></select></label>
  <label><span>qp_delta</span><input type="range" id="qp_delta" min="-20" max="0" value="-8">
    <output class="val" id="qp_delta_v">-8</output></label>
  <label><span>background +qp</span><input type="range" id="background_qp_delta" min="0"
    max="30" value="12"><output class="val" id="background_qp_delta_v">12</output></label>
  <label><span>pad_norm</span><input type="range" id="pad_norm" min="0" max="0.2"
    step="0.005" value="0.02"><output class="val" id="pad_norm_v">0.02</output></label>
  <label><span>hold_sec</span><input type="range" id="hold_sec" min="0.1" max="20"
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
  <div id="views">
    <div class="pane"><h3>original</h3><div class="shot">
      <div class="frame" id="lframe"><img id="left">
        <svg viewBox="0 0 1 1" preserveAspectRatio="none"><g id="lbox"></g></svg></div></div></div>
    <div class="pane"><h3>ROI encoded</h3><div class="shot">
      <div class="frame" id="rframe"><img id="right">
        <svg viewBox="0 0 1 1" preserveAspectRatio="none"><g id="rbox"></g></svg></div></div></div>
  </div>
  <div id="bar">
    <span>frame</span>
    <input type="range" id="frame" min="0" max="0" value="0" disabled>
    <output class="val" id="frame_v">0</output>
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
               "bbox_x","bbox_y","bbox_w","bbox_h","seconds"];
const ALL = KNOBS.concat(["encoder","codec","kind","target_bitrate","metrics","roi_file",
                          "override_quality"]);
let runId = null, poll = null, pending = null, srcInfo = null, current = null;

for (const k of KNOBS) {
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
    const t = index / fps, hold = current.hold_sec || 0.25;
    const live = current.track.filter(b => t >= b.t - 1e-9 && t < b.t + hold);
    return live.map(b => ({x: b.x, y: b.y, w: b.w, h: b.h, p}));
  }
  return [{x: +$("bbox_x").value, y: +$("bbox_y").value,
           w: +$("bbox_w").value, h: +$("bbox_h").value, p}];
}

function drawBox(index) {
  const svg = boxesForFrame(index).map(b => {
    const hx = Math.max(0, b.x - b.p), hy = Math.max(0, b.y - b.p);
    const hw = Math.min(1 - hx, b.w + 2 * b.p), hh = Math.min(1 - hy, b.h + 2 * b.p);
    return `<rect x="${hx}" y="${hy}" width="${hw}" height="${hh}" fill="none"
              stroke="#f5a623" stroke-width="0.003" stroke-dasharray="0.012 0.008"/>` +
           `<rect x="${b.x}" y="${b.y}" width="${b.w}" height="${b.h}" fill="none"
              stroke="#39d353" stroke-width="0.004"/>`;
  }).join("");
  $("lbox").innerHTML = svg; $("rbox").innerHTML = svg;
}
for (const k of ["bbox_x","bbox_y","bbox_w","bbox_h","pad_norm"])
  $(k).addEventListener("input", () => drawBox(Number($("frame").value) || 0));
drawBox(0);

function params() {
  const p = {source: $("source").value, encoder: $("encoder").value, codec: $("codec").value,
             kind: $("kind").value, target_bitrate: Number($("target_bitrate").value),
             metrics: $("metrics").checked, roi_file: $("roi_file").value,
             override_quality: $("override_quality").checked, t: 0.0};
  for (const k of KNOBS) p[k] = Number($(k).value);
  return p;
}
const fmtBytes = n => n > 1e6 ? (n/1e6).toFixed(2)+" MB" : n > 1e3 ? (n/1e3).toFixed(1)+" kB" : n+" B";

async function loadSource() {
  try {
    const r = await fetch("/source?path=" + encodeURIComponent($("source").value));
    const i = await r.json();
    if (i.error) { $("srcinfo").textContent = i.error; srcInfo = null; return; }
    srcInfo = i;
    $("srcinfo").innerHTML =
      `<b>${i.codec}</b> ${i.width}&times;${i.height}, <b>${i.fps_text}</b> fps<br>` +
      `${i.frames} frames, ${i.duration.toFixed(2)} s, ${fmtBytes(i.bytes)}<br>` +
      `${i.keyframes} keyframes &rarr; GOP &asymp; <b>${i.gop}</b>`;
    $("gophint").textContent = "source GOP ≈ " + i.gop;
    $("seconds").max = Math.max(1, Math.ceil(i.duration));
    for (const el of [$("lframe"), $("rframe")])
      el.style.aspectRatio = i.width + " / " + i.height;
  } catch (e) { $("srcinfo").textContent = String(e); }
}
$("source").addEventListener("change", loadSource);
loadSource();

async function loadRoi() {
  const ref = $("roi_file").value.trim();
  if (!ref) { $("roiinfo").textContent = ""; return; }
  const r = await fetch("/roi?path=" + encodeURIComponent(ref));
  const i = await r.json();
  $("roiinfo").textContent = i.error ? i.error
    : `${i.count} samples, kinds: ${i.kinds.join(", ") || "—"}, t ∈ ` +
      `[${i.t_min.toFixed(2)}, ${i.t_max.toFixed(2)}] s`;
}
$("roi_file").addEventListener("change", loadRoi);

function showStats(i) {
  const num = v => v === null || v === undefined ? "—" : Number(v).toFixed(6);
  const ratio = i.clip_bytes ? (100 * i.bytes / i.clip_bytes).toFixed(1) + "%" : "—";
  $("stats").innerHTML = [
    ["size", fmtBytes(i.bytes) + " (" + ratio + " of segment)"],
    ["frames", i.frames + " of " + i.clip_frames],
    ["output", i.codec + " " + i.fps_text + " fps, " + i.duration.toFixed(2) + " s"],
    ["SSIM zone", num(i.ssim_zone)],
    ["SSIM background", num(i.ssim_bg)],
    ["PSNR zone", i.psnr_zone == null ? "—" : Number(i.psnr_zone).toFixed(2) + " dB"],
    ["run time", i.seconds == null ? "—" : i.seconds + " s"],
  ].map(([k, v]) => "<div><b>" + k + "</b>" + v + "</div>").join("");
}

function loadFrame(n) {
  if (!runId) return;
  $("frame_v").value = n;
  $("left").src  = "/img/" + runId + "/src/" + n + ".png";
  $("right").src = "/img/" + runId + "/out/" + n + ".png";
  drawBox(n);
}
$("frame").addEventListener("input", e => loadFrame(Number(e.target.value)));

function busy(on, text) {
  $("prog").classList.toggle("on", on);
  $("stage").textContent = text || "";
  $("go").disabled = on;
}

async function start() {
  $("err").textContent = "";
  busy(true, "queued");
  const r = await fetch("/run", {method: "POST", body: JSON.stringify(params())});
  const {job, error} = await r.json();
  if (error) { busy(false, ""); $("err").textContent = error; return; }
  pending = job;
  if (poll) clearInterval(poll);
  poll = setInterval(async () => {
    const s = await (await fetch("/job?id=" + job)).json();
    if (job !== pending) return;                       // a newer run took over
    busy(!s.done, s.stage);
    if (!s.done) return;
    clearInterval(poll); poll = null; busy(false, "");
    if (s.error) { $("err").textContent = s.error; return; }
    const i = s.result;
    runId = i.id; current = i; showStats(i); $("log").textContent = i.log || "";
    const last = Math.max(0, i.frames - 1);
    const keep = Math.min(Number($("frame").value) || 0, last);
    $("frame").max = last; $("frame").disabled = false; $("frame").value = keep;
    loadFrame(keep);
  }, 250);
}
$("go").addEventListener("click", start);

// Live mode: every knob re-runs after a pause. The pause is what makes it usable -- a
// slider fires on every pixel of travel, and an encode per pixel would queue for minutes.
let timer = null;
for (const k of ALL) $(k).addEventListener("input", () => {
  if (!$("live").checked) return;
  clearTimeout(timer);
  timer = setTimeout(start, 600);
});
$("live").addEventListener("change", () => { if ($("live").checked) start(); });
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
            self._json(source_info(src))
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
                track = track_of(load_roi(ref))
            except Exception as exc:
                self._json({"error": f"cannot read {ref.name}: {exc}"})
                return
            times = [b["t"] for b in track] or [0.0]
            self._json({"count": len(track), "t_min": min(times), "t_max": max(times),
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
            if entry is None or side not in ("src", "out"):
                self._send(404, b"no such run", "text/plain")
                return
            try:
                index = int(name[:-4])
            except ValueError:
                self._send(400, b"bad frame", "text/plain")
                return
            dest = entry["dir"] / "frames" / f"{side}_{index}.png"
            if not extract(entry[side], index, dest):
                self._send(404, b"no such frame", "text/plain")
                return
            self._send(200, dest.read_bytes(), "image/png")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        if urllib.parse.urlparse(self.path).path != "/run":
            self._send(404, b"not found", "text/plain")
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            params = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError as exc:
            self._json({"error": str(exc)}, 400)
            return
        if not CLI.exists():
            self._json({"error": f"roi-transcode not found at {CLI}"}, 500)
            return
        with LOCK:
            COUNTER[0] += 1
            job_id = f"r{COUNTER[0]:04d}"
            JOBS[job_id] = {"id": job_id, "stage": "queued", "done": False,
                            "error": None, "result": None}
        threading.Thread(target=encode_job, args=(job_id, params), daemon=True).start()
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
        print(f"bench on http://127.0.0.1:{PORT}   (runs in {WORK})")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
