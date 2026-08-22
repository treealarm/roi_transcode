# Hardware integration tests — TZ v2.0 §29

`onevpl/` is the one that can run here. `nvenc/` and `rockchip/` are empty for the same
reason their backends are: no hardware.

These are meant to be short — §35 puts them on every merge request at 10–60 seconds per
sequence — so they check that the path opens and behaves, not that quality is good. That
is what `tests/quality/` is for.
