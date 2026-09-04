#!/usr/bin/env python3
"""§41's coverage gate: line coverage of the core modules, from gcov output.

    coverage.py BUILD_DIR SRC_PREFIX...

§41 asks for ">= 80% line coverage of the core modules (src/core, src/roi, src/temporal,
src/metadata)" and says the threshold is checked in CI. This project's layout names those
directories differently -- src/core, src/adaptive, src/metadata -- so the prefixes are given
on the command line rather than guessed here.

**The example application is reported apart from the core rather than counted with it.**
src/core holds two things: the library §41 is talking about, and §52's example application --
transcode_file, session, frame_input, file_mux_sink -- which CMake builds as a separate
target (`sve_examples`) and which the unit tests never touch, because what exercises it is
check.sh driving the CLI. Counting 539 lines of example against the core's gate would report
a number that no change to the library could move.

**Lines are merged across objects, not read off gcov's summary.** A source compiled into
two targets -- src/metadata/cbor.cpp is in the library and in cbor_test -- has one .gcda per
object, and each one only knows what its own binary ran. gcov's own summary for that case is
not a union: handed both files it reported "489" lines for a 163-line source and averaged the
two percentages into it. So each .gcda is converted on its own with `gcov -i` and the per-line
counts are unioned here: a line is covered when any binary executed it, and the denominator is
the source's line count once.

    build:   cmake -S . -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=Debug \
                 -DCMAKE_CXX_FLAGS="--coverage -O0" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    run:     ctest --test-dir build-cov
    report:  integration/scripts/coverage.py build-cov src/core src/adaptive src/metadata
    lines:   integration/scripts/coverage.py --uncovered build-cov src/core/roi_encoder.cpp
"""

# The FFmpeg RAII wrappers: in src/core by history, used by the example application and by
# the libavcodec backend group, and by nothing §41 calls core. Reported on its own line
# rather than excluded, and the reason is worth stating plainly: a rule chosen after seeing
# which side of a threshold it lands on is not a measurement. The number this project
# stands behind is the one computed by the rule that was set first, and taking these lines
# out would move it by a third of a point either way.
FFMPEG_HELPERS = ("src/core/av_raii.h",)

# §52's example application: in src/core by history, not part of what §41 calls core.
EXAMPLE_APPLICATION = (
    "src/core/transcode_file.cpp",
    "src/core/session.cpp",
    "src/core/frame_input.cpp",
    "src/core/frame_input.h",
    "src/core/file_mux_sink.cpp",
    "src/core/file_mux_sink.h",
)
import collections
import gzip
import json
import os
import subprocess
import sys
import tempfile

def line_counts(build, root, prefixes):
    """source path -> {line number: times executed}, unioned over every object."""
    gcda = []
    for dirpath, _dirs, files in os.walk(build):
        gcda += [os.path.abspath(os.path.join(dirpath, f))
                 for f in files if f.endswith(".gcda")]
    if not gcda:
        return None
    hits = collections.defaultdict(dict)
    with tempfile.TemporaryDirectory() as work:
        for one in gcda:
            # Checked rather than assumed: a gcov that fails writes no report, and the
            # object's lines then leave both halves of the ratio at once -- the gate would
            # read as a percentage of whatever still worked, and nothing would say so.
            done = subprocess.run(["gcov", "-i", "-m", "-o", os.path.dirname(one), one],
                                  cwd=work, capture_output=True, text=True)
            if done.returncode != 0:
                raise SystemExit(
                    f"gcov failed on {one} (exit {done.returncode}): {done.stderr.strip()}")
            for name in os.listdir(work):
                with gzip.open(os.path.join(work, name), "rt", encoding="utf-8") as fh:
                    report = json.load(fh)
                # gcno records the source path as the compiler saw it, which for a CMake
                # build is relative to the build directory rather than to the repository.
                base = report.get("current_working_directory", work)
                for entry in report.get("files", []):
                    path = os.path.normpath(os.path.join(base, entry["file"]))
                    rel = os.path.relpath(path, root) if os.path.isabs(path) else path
                    if not any(rel.startswith(p) or ("/" + p) in rel for p in prefixes):
                        continue
                    into = hits[rel]
                    for line in entry.get("lines", []):
                        number = line["line_number"]
                        into[number] = into.get(number, 0) + line["count"]
                os.remove(os.path.join(work, name))
    return hits

def main(argv):
    uncovered_only = argv and argv[0] == "--uncovered"
    if uncovered_only:
        argv = argv[1:]
    build, prefixes = argv[0], argv[1:]
    root = os.path.dirname(os.path.abspath(build.rstrip("/")))
    hits = line_counts(build, root, prefixes)
    if hits is None:
        print("no .gcda files: was the instrumented build run?", file=sys.stderr)
        return 2
    if uncovered_only:
        # Which lines a new test would have to reach. Printed rather than guessed at from
        # a percentage, because "55 lines short" says nothing about what they are.
        for name in sorted(hits):
            missing = sorted(n for n, count in hits[name].items() if count == 0)
            if missing:
                print(f"{name}: {', '.join(str(n) for n in missing)}")
        return 0
    # A header whose inline functions were never instantiated anywhere contributes no
    # lines at all; printing it as 0/0 would be a row that can never move.
    per_file = {name: [sum(1 for c in lines.values() if c > 0), len(lines)]
                for name, lines in hits.items() if lines}
    if not per_file:
        print("no source under " + ", ".join(prefixes), file=sys.stderr)
        return 2
    core = {n: v for n, v in per_file.items() if n not in EXAMPLE_APPLICATION}
    example = {n: v for n, v in per_file.items() if n in EXAMPLE_APPLICATION}
    for name in sorted(core, key=lambda n: core[n][0] / max(core[n][1], 1)):
        c, t = core[name]
        print(f"  {100.0 * c / max(t, 1):6.2f}%  {c:5d}/{t:<5d}  {name}")
    covered = sum(v[0] for v in core.values())
    total = sum(v[1] for v in core.values())
    print(f"\ncore line coverage: {100.0 * covered / max(total, 1):.2f}% "
          f"({covered}/{total} lines, §41 asks for >= 80%)")
    if example:
        e_cov = sum(v[0] for v in example.values())
        e_tot = sum(v[1] for v in example.values())
        print(f"§52 example application, reported apart: "
              f"{100.0 * e_cov / max(e_tot, 1):.2f}% ({e_cov}/{e_tot} lines) -- "
              f"exercised by check.sh through the CLI, not by these tests")
    helpers = {n: v for n, v in core.items() if n in FFMPEG_HELPERS}
    if helpers:
        h_cov = sum(v[0] for v in helpers.values())
        h_tot = sum(v[1] for v in helpers.values())
        print(f"of which FFmpeg RAII helpers (counted above): "
              f"{100.0 * h_cov / max(h_tot, 1):.2f}% ({h_cov}/{h_tot} lines) -- "
              f"used by the example and the libavcodec backends; without them the core "
              f"figure is {100.0 * (covered - h_cov) / max(total - h_tot, 1):.2f}%")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
