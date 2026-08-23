#pragma once

// smart-video-encoder: the one header to include.
//
//   #include "smartvideo/smartvideo.h"
//
// Adaptive encoding -- keep the regions a detector found sharp, and spend fewer bits on
// everything else.
//
// Named after the library rather than after a feature. It was roi.h, from when the whole
// thing was called roi_transcode; a consumer should not have to know that history to find
// the door.
//
// Every other header here is self-contained and can be included on its own -- a caller who
// only writes a PacketHandler needs packet_handler.h and nothing else. The
// public_headers_selfcontained target is what keeps that true: one generated translation
// unit per header, compiled against include/ and nothing else. This header exists so that
// not knowing which one you need is also an answer.
//
// This is the whole public surface. Two ways in -- hand over a file, or push
// frames -- and both take the same Settings and give back the same Report.
// IEncoderBackend (TZ v2.0 §7) comes with it: a reader who wants to know what the
// hardware paths look like should find them here, not by digging through src/.
// Nothing here needs gRPC, Dapr, JSON or anything else from ta_vms; the only
// dependency is FFmpeg, so another project can add_subdirectory(core) and link
// roi_core.
//
//   smartvideo::Report r = smartvideo::transcode_file("in.mp4", "out.mp4", boxes, settings);
//
//   smartvideo::Session s(params, &handler);   // or params.output_path for a file
//   s.submit(frame, regions);
//   smartvideo::Report r = s.finish();

#include "smartvideo/capabilities.h"
#include "smartvideo/encoder_backend.h"
#include "smartvideo/frame_buffer.h"
#include "smartvideo/geometry.h"
#include "smartvideo/memory.h"
#include "smartvideo/object.h"
#include "smartvideo/packet_handler.h"
#include "smartvideo/report.h"
#include "smartvideo/session.h"
#include "smartvideo/types.h"

#include <atomic>
#include <string>
#include <vector>

namespace smartvideo {

// Transcodes `input_path` to `output_path`, keeping `boxes` sharp. Boxes lie on
// the input's timeline and apply for Quality::hold_sec around their own moment.
//
// If `cancel` becomes true mid-run the call throws std::runtime_error.
Report transcode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Settings& settings, std::atomic_bool* cancel = nullptr);

} // namespace smartvideo
