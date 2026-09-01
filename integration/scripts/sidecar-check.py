#!/usr/bin/env python3
"""Does the sidecar correspond to the packets? -- TZ §19.3 and §19.4.

    sidecar-check.py <records.ndjson> <packet-pts.txt>

Three questions, and only the third is hard to pass by accident:

  * one record per packet;
  * frame_index counting emissions from zero;
  * every record's PTS is its packet's.

The last is what the check exists for. Emission order is not submission order once a codec
reorders for B-frames, so records written when a frame was handed in would line up on a
count and disagree here. The caller is expected to assert separately that the stream does
reorder, since on a stream that does not, this passes either way.

The two sides carry different time bases -- the sidecar has the encoder's, the container
its own -- so the comparison is by their ratio, established from the first non-zero PTS.
"""

import json
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    records = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
    packets = [int(x) for x in open(sys.argv[2]) if x.strip()]

    if not records:
        print("no records", file=sys.stderr)
        return 1
    if len(records) != len(packets):
        print(f"{len(records)} records against {len(packets)} packets", file=sys.stderr)
        return 1

    wrong_index = [i for i, r in enumerate(records) if r.get("frame_index") != i]
    if wrong_index:
        print(f"frame_index breaks at {wrong_index[:3]}", file=sys.stderr)
        return 1

    scale = 1
    for record, packet in zip(records, packets):
        if record["pts"]:
            scale = packet // record["pts"]
            break
    mismatched = [(i, r["pts"] * scale, p)
                  for i, (r, p) in enumerate(zip(records, packets))
                  if r["pts"] * scale != p]
    if mismatched:
        print(f"PTS mismatch at {mismatched[:3]} (scale {scale})", file=sys.stderr)
        return 1

    reordered = sum(1 for a, b in zip(records, records[1:]) if b["pts"] < a["pts"])
    print(f"{len(records)} records, scale {scale}, {reordered} reordering points")
    return 0


if __name__ == "__main__":
    sys.exit(main())
