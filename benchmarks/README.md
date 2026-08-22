# Benchmarks — TZ v2.0 §19–§28

`synthetic/` is populated: `make-scenes.py` generates the scenes of §19 deterministically,
each with per-frame ground truth, which is what the ROI metrics are computed against.

`bvi_hd/`, `bdd100k/`, `mot17/` and `ufpr_alpr/` are empty. The datasets are hundreds of
gigabytes and the harness for them is worth building only once there are metrics to feed:
§40 warns that global VMAF cannot be the KPI, so ROI-VMAF against background-VMAF comes
first, and it can be developed entirely on the synthetic corpus.
