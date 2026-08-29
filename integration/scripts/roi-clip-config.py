#!/usr/bin/env python3
"""Turn a roi-clip profile into the arguments roi-clip.sh already understands.

The profile is the command line written down, so there is one vocabulary to learn rather than
two: every key here is a flag there, and the flag still wins when both are given. Arguments are
printed NUL-separated for `mapfile -d ''`, because paths may contain spaces.

    ./roi-clip-config.py profile.yaml | xargs -0 echo     # to see what a profile expands to
"""

import sys

# key -> the flag it becomes. Underscores read better in YAML than the dashes the flags use, so
# both spellings are accepted and normalized to this table.
SCALARS = {
    "outdir": "--outdir",
    "out": "--out",
    "play": "--play",
    "player": "--player",
    "thickness": "--thickness",
    "model_path": "--model-path",
    "encoder": "--encoder",
    "codec": "--codec",
    "crf": "--crf",
    "gop": "--gop",
    "preset": "--preset",
    "background_qp": "--background-qp",
    "pad": "--pad",
    "margin_px": "--margin-px",
    "min_roi_side": "--min-roi-side",
    "max_qp_delta_per_frame": "--max-qp-delta-per-frame",
    "max_regions": "--max-regions",
    "regions": "--regions",
    "mask_scale": "--mask-scale",
    "qpmap_block": "--qpmap-block",
    "detect_fps": "--detect-fps",
    "min_confidence": "--min-confidence",
    "target_bitrate": "--target-bitrate",
    "bitrate_overshoot": "--bitrate-overshoot",
}
FLAGS = {"keep": "--keep"}
KNOWN = set(SCALARS) | set(FLAGS) | {"input", "classes", "qp"}


def die(message):
    sys.exit(f"{sys.argv[0]}: {message}")


def parse_scalar(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    lowered = text.lower()
    if lowered in ("true", "yes", "on"):
        return True
    if lowered in ("false", "no", "off"):
        return False
    for cast in (int, float):
        try:
            return cast(text)
        except ValueError:
            pass
    return text


def load_without_pyyaml(text, path):
    """The flat subset this profile needs: `key: value`, `key: [a, b]`, and an indented block of
    `key: value` under a key with no value of its own. Anything else is refused by name rather
    than guessed at -- a profile that is quietly half-read is worse than one that will not load."""
    result = {}
    block = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split(" #", 1)[0].rstrip() if " #" in raw else raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indented = line[0] in " \t"
        if ":" not in line:
            die(f"{path}:{number}: expected 'key: value', got '{line.strip()}'")
        key, _, value = line.partition(":")
        key, value = key.strip(), value.strip()
        if indented:
            if block is None:
                die(f"{path}:{number}: indented '{key}' does not belong to anything")
            result[block][key] = parse_scalar(value)
            continue
        block = None
        if not value:
            block = key
            result[key] = {}
        elif value.startswith("[") and value.endswith("]"):
            result[key] = [parse_scalar(v) for v in value[1:-1].split(",") if v.strip()]
        else:
            result[key] = parse_scalar(value)
    return result


def load(path):
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as error:
        die(f"cannot read {path}: {error}")
    try:
        import yaml
    except ImportError:
        return load_without_pyyaml(text, path)
    try:
        return yaml.safe_load(text) or {}
    except yaml.YAMLError as error:
        die(f"{path}: {error}")


def render(value):
    if isinstance(value, bool):  # only reachable for a scalar written as a bool by mistake
        return "true" if value else "false"
    return str(value)


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} PROFILE.yaml")
    profile = load(sys.argv[1])
    if not isinstance(profile, dict):
        die(f"{sys.argv[1]}: expected a mapping of settings at the top level")

    args = []
    normalized = {}
    for key, value in profile.items():
        name = str(key).replace("-", "_")
        if name not in KNOWN:
            die(f"{sys.argv[1]}: unknown setting '{key}'\nknown: {', '.join(sorted(KNOWN))}")
        normalized[name] = value

    # The input is positional, and first, so that a path given on the command line lands after it
    # and wins the same way every other setting does.
    if "input" in normalized:
        args.append(render(normalized["input"]))

    for name, flag in SCALARS.items():
        if name in normalized and normalized[name] is not None:
            args += [flag, render(normalized[name])]

    for name, flag in FLAGS.items():
        if normalized.get(name):
            args.append(flag)

    classes = normalized.get("classes")
    if classes is not None:
        if isinstance(classes, str):
            args += ["--classes", classes]
        else:
            args += ["--classes", ",".join(render(c) for c in classes)]

    qp = normalized.get("qp") or {}
    if not isinstance(qp, dict):
        die(f"{sys.argv[1]}: 'qp' wants a block of 'kind: delta' pairs")
    for kind, delta in qp.items():
        args += ["--qp", f"{kind}={render(delta)}"]

    sys.stdout.write("".join(a + "\0" for a in args))


if __name__ == "__main__":
    main()
