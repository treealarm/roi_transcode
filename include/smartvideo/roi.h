#pragma once

// ROI-aware HEVC encoding: keep the regions a detector found sharp, and spend
// fewer bits on everything else.
//
// This is the whole public surface. Two ways in -- hand over a file, or push
// frames -- and both take the same Settings and give back the same Report.
// Nothing here needs gRPC, Dapr, JSON or anything else from ta_vms; the only
// dependency is FFmpeg, so another project can add_subdirectory(core) and link
// roi_core.
//
//   roi::Report r = roi::transcode_file("in.mp4", "out.mp4", boxes, settings);
//
//   roi::Session s(params, &handler);   // or params.output_path for a file
//   s.submit(frame, regions);
//   roi::Report r = s.finish();

#include "smartvideo/geometry.h"
#include "smartvideo/packet_handler.h"
#include "smartvideo/report.h"
#include "smartvideo/session.h"
#include "smartvideo/types.h"

#include <atomic>
#include <string>
#include <vector>

namespace roi {

// Transcodes `input_path` to `output_path`, keeping `boxes` sharp. Boxes lie on
// the input's timeline and apply for Quality::hold_sec around their own moment.
//
// If `cancel` becomes true mid-run the call throws std::runtime_error.
Report transcode_file(const std::string& input_path,
    const std::string& output_path, const std::vector<Box>& boxes,
    const Settings& settings, std::atomic_bool* cancel = nullptr);

} // namespace roi
