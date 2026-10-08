#pragma once

// ta_roienc: the one header to include.
//
//   #include "ta_roienc/ta_roienc.h"
//
// Adaptive encoding -- keep the regions a detector found sharp, and spend fewer bits on
// everything else.
//
// One header is deliberately not pulled in from here, because it names an AVFrame:
//
//   #include "ta_roienc/frame.h"          // AVFrame interop
//
// Everything reachable from this header compiles with include/ and nothing else on the
// path, which is what lets an embedder write a PacketHandler -- or submit frames -- without
// libavutil. A caller who has AVFrames adds one include and everyone else saves a
// dependency.
//
// Every other header here is self-contained and can be included on its own -- a caller who
// only writes a PacketHandler needs packet_handler.h and nothing else. The
// public_headers_selfcontained target is what keeps that true: one generated translation
// unit per header, compiled against include/ and nothing else. This header exists so that
// not knowing which one you need is also an answer.
//
// Two ways in -- hand over a file, or push frames -- and both take the same Settings and
// give back the same Report. IEncoderBackend comes with it: a reader who wants to know
// what the hardware paths look like should find them here, not by digging through src/.
// The only dependency is FFmpeg, so another project can add_subdirectory() this directory
// and link ta_roienc.
//
//   ta_roienc::Report r = ta_roienc::transcode_file("in.mp4", "out.mp4", boxes, settings);
//
//   ta_roienc::Session s(params, &handler);   // or params.output_path for a file
//   s.submit(frame, regions);
//   ta_roienc::Report r = s.finish();

#include "ta_roienc/backend_registry.h"
#include "ta_roienc/capabilities.h"
#include "ta_roienc/codec_parameters.h"
#include "ta_roienc/encoder.h"
#include "ta_roienc/encoder_backend.h"
#include "ta_roienc/frame_buffer.h"
#include "ta_roienc/frame_metadata.h"
#include "ta_roienc/geometry.h"
#include "ta_roienc/logging.h"
#include "ta_roienc/memory.h"
#include "ta_roienc/metadata.h"
#include "ta_roienc/object.h"
#include "ta_roienc/object_priority.h"
#include "ta_roienc/packet.h"
#include "ta_roienc/packet_handler.h"
#include "ta_roienc/raw_frame.h"
#include "ta_roienc/report.h"
#include "ta_roienc/session.h"
#include "ta_roienc/timed_frame.h"
#include "ta_roienc/timestamp.h"
#include "ta_roienc/types.h"

#include <atomic>
#include <string>
#include <vector>

namespace ta_roienc {

// Transcodes `input_path` to `output_path`, keeping `boxes` sharp. Boxes lie on
// the input's timeline and apply for Quality::hold_sec around their own moment.
//
// If `cancel` becomes true mid-run the call throws std::runtime_error.
Report transcode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Settings& settings, std::atomic_bool* cancel = nullptr);

} // namespace ta_roienc

