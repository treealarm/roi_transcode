#!/usr/bin/env python3
"""A bench for judging ROI encoding by eye: original on the left, ours on the right.

The metrics this project already computes -- SSIM and PSNR over the zone, the background
and the ring around the edge -- answer whether the quality moved where it was asked to.
They do not answer whether the result looks right, and STATUS.md records that gap plainly
under Р1: VMAF is deferred, and "видно ли это глазу" has had no instrument at all. This is
that instrument.

No external dependencies, and that is deliberate rather than minimal. Р1 keeps heavy
dependencies out of the checking loop and Р2 keeps the tree portable, so a Qt or a pip
install would break both for a tool nobody needs to build. The standard library serves a
page; the browser is the toolkit.

It drives the roi-transcode CLI for now. When §58's Python bindings land, only run_encode
below changes -- the page, the knobs and the comparison do not.

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
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD", ROOT / "build" / "integration"))
CLI = BUILD / "roi-transcode"
PORT = int(os.environ.get("PORT", "8765"))

# Runs live here and are thrown away with the process. Nothing is written into the tree.
WORK = Path(tempfile.mkdtemp(prefix="sve-bench-"))
RUNS: dict[str, dict] = {}
LOCK = threading.Lock()


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def probe(path: Path, entries: str) -> str:
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0",
               "-show_entries", entries, "-of", "csv=p=0", str(path)])
    return out.stdout.strip()


def frame_count(path: Path) -> int:
    out = run(["ffprobe", "-v", "error", "-count_packets", "-select_streams", "v:0",
               "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", str(path)])
    try:
        return int(out.stdout.strip())
    except ValueError:
        return 0


def metric(kind: str, src: Path, enc: Path, crop: tuple[int, int, int, int]) -> float | None:
    """SSIM or PSNR over one rectangle, the same way check.sh measures it.

    Both files are cropped identically and compared frame by frame, so this is the whole
    clip rather than a sample -- a zone that is protected for the first second and dropped
    afterwards should not be able to score well here.
    """
    x, y, w, h = crop
    filt = (f"[0:v]crop={w}:{h}:{x}:{y}[a];[1:v]crop={w}:{h}:{x}:{y}[b];[a][b]{kind}")
    out = run(["ffmpeg", "-hide_banner", "-i", str(src), "-i", str(enc),
               "-filter_complex", filt, "-f", "null", "-"])
    marker = "All:" if kind == "ssim" else "average:"
    for line in reversed(out.stderr.splitlines()):
        if marker in line:
            tail = line.split(marker, 1)[1].split()[0]
            try:
                return float(tail)
            except ValueError:
                return None
    return None


def write_roi(path: Path, p: dict) -> None:
    """The knobs the CLI does not take as flags travel in the ROI document."""
    doc = {
        "background_qp_delta": p["background_qp_delta"],
        "hold_sec": p["hold_sec"],
        "kinds": {p["kind"]: {"qp_delta": p["qp_delta"], "pad_norm": p["pad_norm"]}},
        "samples": [{
            "t": p["t"],
            "kind": p["kind"],
            "bbox_x": p["bbox_x"], "bbox_y": p["bbox_y"],
            "bbox_w": p["bbox_w"], "bbox_h": p["bbox_h"],
        }],
    }
    if p.get("max_protected_fraction"):
        doc["max_protected_fraction"] = p["max_protected_fraction"]
    path.write_text(json.dumps(doc, indent=2))


def run_encode(p: dict) -> dict:
    """One encode, and everything measured about it.

    The single place that knows how the library is invoked. §58's bindings replace the
    subprocess here and nothing above or below it changes.
    """
    src = Path(p["source"])
    if not src.is_absolute():
        src = ROOT / src
    if not src.exists():
        return {"error": f"source not found: {src}"}
    if not CLI.exists():
        return {"error": f"roi-transcode not found at {CLI}; set BUILD="}

    run_id = f"r{len(RUNS) + 1:04d}"
    rundir = WORK / run_id
    (rundir / "frames").mkdir(parents=True, exist_ok=True)
    roi_path = rundir / "roi.json"
    out_path = rundir / "out.mp4"
    write_roi(roi_path, p)

    cmd = [str(CLI), "--in", str(src), "--roi", str(roi_path), "--out", str(out_path),
           "--encoder", p["encoder"], "--codec", p["codec"],
           "--crf", str(p["crf"]), "--gop", str(p["gop"])]
    if p.get("preset"):
        cmd += ["--preset", p["preset"]]
    if int(p.get("target_bitrate") or 0) > 0:
        cmd += ["--target-bitrate", str(int(p["target_bitrate"]))]
    proc = run(cmd)
    log = (proc.stdout + proc.stderr).strip()
    if proc.returncode != 0 or not out_path.exists():
        return {"error": f"encode failed (rc={proc.returncode})", "log": log}

    wh = probe(src, "stream=width,height").split(",")
    width, height = (int(wh[0]), int(wh[1])) if len(wh) == 2 else (0, 0)

    # The measured rectangles follow the box the user placed, so moving it moves the
    # measurement with it. The background sample is deliberately a corner the box cannot
    # reach, which is what makes "the background gave up quality" a fair reading.
    zx = int(p["bbox_x"] * width)
    zy = int(p["bbox_y"] * height)
    zw = max(16, int(p["bbox_w"] * width) // 2 * 2)
    zh = max(16, int(p["bbox_h"] * height) // 2 * 2)
    zone = (min(zx, width - zw), min(zy, height - zh), zw, zh)
    bg = (16, 16, zw, zh)

    info = {
        "id": run_id,
        "bytes": out_path.stat().st_size,
        "source_bytes": src.stat().st_size,
        "frames": frame_count(out_path),
        "source_frames": frame_count(src),
        "width": width, "height": height,
        "codec": probe(out_path, "stream=codec_name"),
        "zone": zone, "bg": bg,
        "ssim_zone": metric("ssim", src, out_path, zone),
        "ssim_bg": metric("ssim", src, out_path, bg),
        "psnr_zone": metric("psnr", src, out_path, zone),
        "log": log,
    }
    with LOCK:
        RUNS[run_id] = {"src": src, "out": out_path, "dir": rundir, "info": info}
    return info


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
  :root { color-scheme: light dark; --line:#8883; --bg:#fff; --fg:#111; --panel:#f6f6f7; }
  @media (prefers-color-scheme: dark) { :root { --bg:#16171a; --fg:#e8e8ea; --panel:#1e2024; } }
  * { box-sizing: border-box; }
  body { margin:0; font:13px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
         background:var(--bg); color:var(--fg); display:flex; height:100vh; }
  #side { width:320px; flex:0 0 320px; overflow-y:auto; padding:14px; background:var(--panel);
          border-right:1px solid var(--line); }
  #main { flex:1; display:flex; flex-direction:column; min-width:0; padding:14px; gap:10px; }
  h1 { font-size:14px; margin:0 0 10px; letter-spacing:.02em; }
  h2 { font-size:11px; text-transform:uppercase; letter-spacing:.08em; opacity:.6;
       margin:16px 0 6px; }
  label { display:flex; align-items:center; justify-content:space-between; gap:8px;
          margin:6px 0; }
  label span { white-space:nowrap; }
  input[type=range] { flex:1; min-width:0; }
  input[type=number], select { width:96px; padding:3px 5px; background:var(--bg);
          color:var(--fg); border:1px solid var(--line); border-radius:4px; }
  .val { width:52px; text-align:right; font-variant-numeric:tabular-nums; opacity:.75; }
  button { width:100%; padding:9px; margin-top:14px; font:inherit; font-weight:600;
           border:1px solid var(--line); border-radius:6px; background:var(--fg);
           color:var(--bg); cursor:pointer; }
  button:disabled { opacity:.5; cursor:default; }
  #views { flex:1; display:flex; gap:10px; min-height:0; }
  .pane { flex:1; display:flex; flex-direction:column; min-width:0; }
  .pane h3 { font-size:11px; text-transform:uppercase; letter-spacing:.08em; opacity:.6;
             margin:0 0 5px; }
  .shot { flex:1; min-height:0; background:#0004; border:1px solid var(--line);
          border-radius:6px; display:flex; align-items:center; justify-content:center; }
  .shot img { max-width:100%; max-height:100%; object-fit:contain; display:block; }
  #stats { display:flex; flex-wrap:wrap; gap:8px 20px; font-variant-numeric:tabular-nums; }
  #stats div { min-width:110px; }
  #stats b { display:block; font-size:10px; text-transform:uppercase; letter-spacing:.06em;
             opacity:.55; font-weight:600; }
  #log { white-space:pre-wrap; font:11px/1.4 ui-monospace,monospace; max-height:120px;
         overflow:auto; opacity:.7; border-top:1px solid var(--line); padding-top:8px; }
  #err { color:#c0392b; font-weight:600; }
</style>
<div id="side">
  <h1>ROI encoder bench</h1>

  <h2>Источник</h2>
  <label><span>клип</span><input type="text" id="source" value="testdata/synth.mp4"
      style="width:150px"></label>

  <h2>Кодирование</h2>
  <label><span>backend</span><select id="encoder">
    <option>cpu</option><option>x264</option><option>x264direct</option>
    <option>x265</option><option>x265direct</option><option>qsv</option>
    <option>vaapi</option><option>auto</option></select></label>
  <label><span>codec</span><select id="codec">
    <option>h264</option><option selected>h265</option></select></label>
  <label><span>crf</span><input type="range" id="crf" min="10" max="45" value="28">
    <output class="val" id="crf_v">28</output></label>
  <label><span>gop</span><input type="range" id="gop" min="1" max="120" value="15">
    <output class="val" id="gop_v">15</output></label>
  <label><span>target bps</span><input type="number" id="target_bitrate" value="0"
    step="100000" min="0"></label>

  <h2>ROI</h2>
  <label><span>kind</span><select id="kind">
    <option>plate</option><option>face</option><option>person</option></select></label>
  <label><span>qp_delta</span><input type="range" id="qp_delta" min="-20" max="0" value="-8">
    <output class="val" id="qp_delta_v">-8</output></label>
  <label><span>фон +qp</span><input type="range" id="background_qp_delta" min="0" max="30"
    value="12"><output class="val" id="background_qp_delta_v">12</output></label>
  <label><span>pad_norm</span><input type="range" id="pad_norm" min="0" max="0.2"
    step="0.005" value="0.02"><output class="val" id="pad_norm_v">0.02</output></label>
  <label><span>hold_sec</span><input type="range" id="hold_sec" min="0.1" max="20"
    step="0.1" value="20"><output class="val" id="hold_sec_v">20</output></label>

  <h2>Рамка (доли кадра)</h2>
  <label><span>x</span><input type="range" id="bbox_x" min="0" max="0.9" step="0.01"
    value="0.35"><output class="val" id="bbox_x_v">0.35</output></label>
  <label><span>y</span><input type="range" id="bbox_y" min="0" max="0.9" step="0.01"
    value="0.40"><output class="val" id="bbox_y_v">0.40</output></label>
  <label><span>w</span><input type="range" id="bbox_w" min="0.02" max="1" step="0.01"
    value="0.30"><output class="val" id="bbox_w_v">0.30</output></label>
  <label><span>h</span><input type="range" id="bbox_h" min="0.02" max="1" step="0.01"
    value="0.20"><output class="val" id="bbox_h_v">0.20</output></label>

  <button id="go">Кодировать</button>
</div>

<div id="main">
  <div id="views">
    <div class="pane"><h3>оригинал</h3><div class="shot"><img id="left"></div></div>
    <div class="pane"><h3 id="rlabel">после ROI-энкодера</h3>
      <div class="shot"><img id="right"></div></div>
  </div>
  <label><span>кадр</span><input type="range" id="frame" min="0" max="0" value="0"
    disabled><output class="val" id="frame_v">0</output></label>
  <div id="stats"></div>
  <div id="err"></div>
  <div id="log"></div>
</div>

<script>
const $ = id => document.getElementById(id);
const KNOBS = ["crf","gop","qp_delta","background_qp_delta","pad_norm","hold_sec",
               "bbox_x","bbox_y","bbox_w","bbox_h"];
for (const k of KNOBS) {
  const el = $(k), out = $(k + "_v");
  const sync = () => out.value = el.value;
  el.addEventListener("input", sync); sync();
}
let runId = null;

function params() {
  const p = {source: $("source").value, encoder: $("encoder").value,
             codec: $("codec").value, kind: $("kind").value,
             target_bitrate: Number($("target_bitrate").value), t: 0.0};
  for (const k of KNOBS) p[k] = Number($(k).value);
  return p;
}

function fmtBytes(n) {
  if (n > 1e6) return (n / 1e6).toFixed(2) + " MB";
  if (n > 1e3) return (n / 1e3).toFixed(1) + " kB";
  return n + " B";
}

function showStats(i) {
  const num = v => v === null || v === undefined ? "—" : Number(v).toFixed(6);
  const ratio = i.source_bytes ? (100 * i.bytes / i.source_bytes).toFixed(1) + "%" : "—";
  $("stats").innerHTML = [
    ["размер", fmtBytes(i.bytes) + " (" + ratio + " от исходного)"],
    ["кадров", i.frames + " из " + i.source_frames],
    ["кодек", i.codec + " " + i.width + "×" + i.height],
    ["SSIM в зоне", num(i.ssim_zone)],
    ["SSIM фона", num(i.ssim_bg)],
    ["PSNR в зоне", i.psnr_zone === null ? "—" : Number(i.psnr_zone).toFixed(2) + " dB"],
  ].map(([k, v]) => "<div><b>" + k + "</b>" + v + "</div>").join("");
}

function loadFrame(n) {
  if (!runId) return;
  $("frame_v").value = n;
  $("left").src  = "/img/" + runId + "/src/" + n + ".png";
  $("right").src = "/img/" + runId + "/out/" + n + ".png";
}
$("frame").addEventListener("input", e => loadFrame(Number(e.target.value)));

$("go").addEventListener("click", async () => {
  $("go").disabled = true; $("go").textContent = "Кодирую…";
  $("err").textContent = ""; $("log").textContent = "";
  try {
    const r = await fetch("/run", {method: "POST", body: JSON.stringify(params())});
    const i = await r.json();
    if (i.error) { $("err").textContent = i.error; $("log").textContent = i.log || ""; }
    else {
      runId = i.id; showStats(i); $("log").textContent = i.log || "";
      const last = Math.max(0, i.frames - 1);
      $("frame").max = last; $("frame").disabled = false;
      const mid = Math.min(last, Math.round(last / 2));
      $("frame").value = mid; loadFrame(mid);
    }
  } catch (e) { $("err").textContent = String(e); }
  $("go").disabled = false; $("go").textContent = "Кодировать";
});
</script>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet; the encode log is what matters
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode(), "text/html; charset=utf-8")
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
            if not extract(entry[src_key(side)], index, dest):
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
            self._send(400, json.dumps({"error": str(exc)}).encode(), "application/json")
            return
        result = run_encode(params)
        self._send(200, json.dumps(result).encode(), "application/json")


def src_key(side: str) -> str:
    return "src" if side == "src" else "out"


def main() -> int:
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            print(f"{tool} is not on PATH", file=sys.stderr)
            return 1
    if not CLI.exists():
        print(f"roi-transcode not found at {CLI}\n"
              f"build it with SVE_BUILD_INTEGRATION=ON, or set BUILD=", file=sys.stderr)
        return 1
    with socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler) as srv:
        srv.allow_reuse_address = True
        print(f"bench on http://127.0.0.1:{PORT}   (runs in {WORK})")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
