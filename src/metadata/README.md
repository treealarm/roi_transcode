# SEI metadata — not implemented

TZ v2.0 §16: user-data-unregistered SEI carrying frame_id, PTS, track_id, class_id,
confidence, geometry and priority, in full / delta / periodic-checkpoint forms.

Deferred deliberately rather than for lack of hardware: ta_vms already stores detections in
its own database beside the clip, so nothing consumes this yet. It becomes necessary the
moment a stream is handed to a third party that has no access to that database.
