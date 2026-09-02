#!/usr/bin/env python3
"""Every enumerator C++ has, the Python module must have -- §58.3.

tests/unit/enum_surface.cpp prints one line per enumerator of every enum the bindings
expose; this reads that dump and checks the bound enums against it, in both directions.

**Values are compared, and names are derived rather than listed.** The two languages spell
the same enumerator differently on purpose -- `IntelOneVplDirect` against
`INTEL_ONE_VPL_DIRECT` -- so the check converts one spelling to the other with a rule: a
capital that follows a lower-case letter or a digit starts a new word. A future enumerator
that the rule cannot convert is reported rather than skipped, because a name nobody can
predict is a name a caller has to look up, which is the thing the rule buys.

Run with the package importable and the dump on stdin:

    build/enum_surface | PYTHONPATH=python python3 integration/scripts/python-enum-parity.py
"""

import re
import sys

import smart_video_encoder as sve


def screaming(cpp_name: str) -> str:
    """`IntelOneVplDirect` -> `INTEL_ONE_VPL_DIRECT`, `YUV420P` -> `YUV420P`.

    A capital starts a new word when it follows a letter or digit *and* is followed by a
    lower-case letter. The second half is what keeps the trailing P of `YUV420P` attached
    while still splitting `X264Direct`: one is an initialism, the other a word.
    """
    return re.sub(r"(?<=[a-zA-Z0-9])([A-Z])(?=[a-z])", r"_\1", cpp_name).upper()


def main() -> int:
    dumped: dict[str, dict[int, str]] = {}
    for line in sys.stdin.read().splitlines():
        if not line.strip():
            continue
        enum, value, name = line.split()
        dumped.setdefault(enum, {})[int(value)] = name
    if not dumped:
        print("python-enum-parity: no dump on stdin", file=sys.stderr)
        return 2

    problems: list[str] = []
    for enum, members in sorted(dumped.items()):
        bound = getattr(sve, enum, None)
        if bound is None:
            problems.append(f"{enum}: the C++ enum is not bound at all")
            continue
        # `__members__` rather than iteration: a pybind11 enum is a type, not a Python
        # enum.Enum, and §58.3 asked for exactly that (an enum.Enum shim would be a second
        # vocabulary to keep in step with this one).
        by_value = {int(member.value): name for name, member in bound.__members__.items()}

        for value, cpp_name in sorted(members.items()):
            if value not in by_value:
                problems.append(
                    f"{enum}.{cpp_name} (= {value}) is in C++ and not in the bindings")
            elif by_value[value] != screaming(cpp_name):
                problems.append(
                    f"{enum} value {value} is {cpp_name} in C++ and "
                    f"{by_value[value]} in the bindings, which is not what the naming "
                    f"rule gives ({screaming(cpp_name)})")
        for value, py_name in sorted(by_value.items()):
            if value not in members:
                problems.append(
                    f"{enum}.{py_name} (= {value}) is bound and is no enumerator in C++")

    total = sum(len(v) for v in dumped.values())
    if problems:
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        print(f"python-enum-parity: {len(problems)} of {total} enumerators disagree",
              file=sys.stderr)
        return 1
    print(f"python-enum-parity: {total} enumerators across {len(dumped)} enums agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
