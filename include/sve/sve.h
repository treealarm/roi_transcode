#pragma once

// smart-video-encoder: the one header to include.
//
//   #include "sve/sve.h"
//
// Adaptive encoding -- keep the regions a detector found sharp, and spend fewer bits on
// everything else.
//
// Named after the library rather than after a feature. It was roi.h, from when the whole
// thing was called roi_transcode, and smartvideo.h after that; a consumer should not have
// to know that history to find the door.
//
// Two headers are deliberately not pulled in from here, because both name an AVFrame:
//
//   #include "sve/frame.h"          // owning frames and raw-plane views (§8.5, §8.10)
//   #include "sve/push_encoder.h"   // the callback API (§8.8), which takes one
//
// Everything reachable from this header compiles with include/ and nothing else on the
// path, which is what lets an embedder write a PacketHandler without libavutil. A caller
// who submits frames has FFmpeg already -- §55 makes AVFrame the input interface -- so for
// them the extra include costs nothing, and for everyone else it saves a dependency.
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
// dependency is FFmpeg, so another project can add_subdirectory() this directory
// and link sve.
//
//   sve::Report r = sve::transcode_file("in.mp4", "out.mp4", boxes, settings);
//
//   sve::Session s(params, &handler);   // or params.output_path for a file
//   s.submit(frame, regions);
//   sve::Report r = s.finish();

#include "sve/capabilities.h"
#include "sve/codec_parameters.h"
#include "sve/encoder.h"
#include "sve/encoder_backend.h"
#include "sve/frame_buffer.h"
#include "sve/frame_metadata.h"
#include "sve/geometry.h"
#include "sve/logging.h"
#include "sve/memory.h"
#include "sve/metadata.h"
#include "sve/object.h"
#include "sve/object_priority.h"
#include "sve/packet.h"
#include "sve/packet_handler.h"
#include "sve/report.h"
#include "sve/session.h"
#include "sve/timestamp.h"
#include "sve/types.h"

#include <atomic>
#include <string>
#include <vector>

namespace sve {

// Transcodes `input_path` to `output_path`, keeping `boxes` sharp. Boxes lie on
// the input's timeline and apply for Quality::hold_sec around their own moment.
//
// If `cancel` becomes true mid-run the call throws std::runtime_error.
Report transcode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Settings& settings, std::atomic_bool* cancel = nullptr);

} // namespace sve

// Transitional, added 2026-08-30 with the rename to `sve`. TZ v3.4 §23 links the capability
// CLI against a library called `sve`, and the namespace follows the library. Nothing outside
// roi_transcode/ ever spelled `smartvideo::`, so this alias exists only to keep integration/
// and the tests compiling across the slices of the §8 rewrite that follow. It goes away with
// the CMake aliases once those call sites move to the new API -- do not write new code
// against it.
namespace smartvideo = sve;
