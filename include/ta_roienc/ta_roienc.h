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
//   #include "ta_roienc/picture.h"          // AVFrame interop
//
// Everything reachable from this header compiles with include/ and nothing else on the
// path, which is what lets an embedder write a AccessUnitSink -- or submit frames -- without
// libavutil. A caller who has AVFrames adds one include and everyone else saves a
// dependency.
//
// Every other header here is self-contained and can be included on its own -- a caller who
// only writes a AccessUnitSink needs access_unit_sink.h and nothing else. The
// public_headers_selfcontained target is what keeps that true: one generated translation
// unit per header, compiled against include/ and nothing else. This header exists so that
// not knowing which one you need is also an answer.
//
// Two ways in -- hand over a file, or push frames -- and both take the same Recipe and
// give back the same Outcome. IEngine comes with it: a reader who wants to know
// what the hardware paths look like should find them here, not by digging through src/.
// The only dependency is FFmpeg, so another project can add_subdirectory() this directory
// and link ta_roienc.
//
//   ta_roienc::Outcome r = ta_roienc::recode_file("in.mp4", "out.mp4", boxes, settings);
//
//   ta_roienc::Transcoder s(params, &handler);   // or params.output_path for a file
//   s.submit(frame, regions);
//   ta_roienc::Outcome r = s.finish();

#include "ta_roienc/engine_catalog.h"
#include "ta_roienc/abilities.h"
#include "ta_roienc/coding_params.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/engine.h"
#include "ta_roienc/picture_store.h"
#include "ta_roienc/picture_info.h"
#include "ta_roienc/shapes.h"
#include "ta_roienc/log.h"
#include "ta_roienc/memory_kind.h"
#include "ta_roienc/metadata_file.h"
#include "ta_roienc/tracked_object.h"
#include "ta_roienc/salience.h"
#include "ta_roienc/access_unit.h"
#include "ta_roienc/access_unit_sink.h"
#include "ta_roienc/picture_view.h"
#include "ta_roienc/outcome.h"
#include "ta_roienc/transcoder.h"
#include "ta_roienc/stamped_picture.h"
#include "ta_roienc/stamp.h"
#include "ta_roienc/basics.h"

#include <atomic>
#include <string>
#include <vector>

namespace ta_roienc {

// Transcodes `input_path` to `output_path`, keeping `boxes` sharp. Boxes lie on
// the input's timeline and apply for QualityPlan::hold_sec around their own moment.
//
// If `cancel` becomes true mid-run the call throws std::runtime_error.
Outcome recode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Recipe& settings, std::atomic_bool* cancel = nullptr);

} // namespace ta_roienc

