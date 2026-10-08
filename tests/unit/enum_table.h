#pragma once

// Every enumerator of every enum the public API exposes, spelled out once.
//
// names_test asks the library's own *_name() function for each value; keeping the list here
// rather than inside the test is what lets a new enum be added in one obvious place.
//
// Each enum is a switch with no `default:`, so adding an enumerator makes the compiler warn
// here (-Wswitch, in -Wall). That warning is the first line of defence; names_test is the
// second.

#include "ta_roienc/backend_selection.h"
#include "ta_roienc/encoder.h"
#include "ta_roienc/frame_buffer.h"
#include "ta_roienc/logging.h"
#include "ta_roienc/object_priority.h"
#include "ta_roienc/policy.h"
#include "ta_roienc/status.h"
#include "ta_roienc/types.h"

namespace ta_roienc_enum_table {

inline const char* spell(ta_roienc::Backend e)
{
    switch (e) {
    case ta_roienc::Backend::Auto:              return "Auto";
    case ta_roienc::Backend::IntelOneVpl:       return "IntelOneVpl";
    case ta_roienc::Backend::Vaapi:             return "Vaapi";
    case ta_roienc::Backend::X264:              return "X264";
    case ta_roienc::Backend::X265:              return "X265";
    case ta_roienc::Backend::X264Direct:        return "X264Direct";
    case ta_roienc::Backend::X265Direct:        return "X265Direct";
    case ta_roienc::Backend::IntelOneVplDirect: return "IntelOneVplDirect";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::StatusCode e)
{
    switch (e) {
    case ta_roienc::StatusCode::Ok:                return "Ok";
    case ta_roienc::StatusCode::DeviceUnavailable: return "DeviceUnavailable";
    case ta_roienc::StatusCode::Unsupported:       return "Unsupported";
    case ta_roienc::StatusCode::InvalidArgument:   return "InvalidArgument";
    case ta_roienc::StatusCode::Again:             return "Again";
    case ta_roienc::StatusCode::EndOfStream:       return "EndOfStream";
    case ta_roienc::StatusCode::BackendError:      return "BackendError";
    case ta_roienc::StatusCode::OutOfMemory:       return "OutOfMemory";
    case ta_roienc::StatusCode::Timeout:           return "Timeout";
    case ta_roienc::StatusCode::Error:             return "Error";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::PixelFormat e)
{
    switch (e) {
    case ta_roienc::PixelFormat::Unknown:         return "Unknown";
    case ta_roienc::PixelFormat::YUV420P:         return "YUV420P";
    case ta_roienc::PixelFormat::NV12:            return "NV12";
    case ta_roienc::PixelFormat::YUV422P:         return "YUV422P";
    case ta_roienc::PixelFormat::YUV444P:         return "YUV444P";
    case ta_roienc::PixelFormat::GRAY8:           return "GRAY8";
    case ta_roienc::PixelFormat::P010:            return "P010";
    case ta_roienc::PixelFormat::YUV420P10:       return "YUV420P10";
    case ta_roienc::PixelFormat::HardwareSurface: return "HardwareSurface";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::Codec e)
{
    switch (e) {
    case ta_roienc::Codec::H264:         return "H264";
    case ta_roienc::Codec::H265:         return "H265";
    case ta_roienc::Codec::SameAsSource: return "SameAsSource";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::LogLevel e)
{
    switch (e) {
    case ta_roienc::LogLevel::Trace:   return "Trace";
    case ta_roienc::LogLevel::Debug:   return "Debug";
    case ta_roienc::LogLevel::Info:    return "Info";
    case ta_roienc::LogLevel::Warning: return "Warning";
    case ta_roienc::LogLevel::Error:   return "Error";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::ObjectPriority e)
{
    switch (e) {
    case ta_roienc::ObjectPriority::Background: return "Background";
    case ta_roienc::ObjectPriority::Low:        return "Low";
    case ta_roienc::ObjectPriority::Normal:     return "Normal";
    case ta_roienc::ObjectPriority::High:       return "High";
    case ta_roienc::ObjectPriority::Critical:   return "Critical";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::EncoderState e)
{
    switch (e) {
    case ta_roienc::EncoderState::Created:     return "Created";
    case ta_roienc::EncoderState::Configured:  return "Configured";
    case ta_roienc::EncoderState::Running:     return "Running";
    case ta_roienc::EncoderState::Draining:    return "Draining";
    case ta_roienc::EncoderState::EndOfStream: return "EndOfStream";
    case ta_roienc::EncoderState::Stopped:     return "Stopped";
    case ta_roienc::EncoderState::Failed:      return "Failed";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::SidecarFormat e)
{
    switch (e) {
    case ta_roienc::SidecarFormat::None:   return "None";
    case ta_roienc::SidecarFormat::NDJSON: return "NDJSON";
    }
    return nullptr;
}

/// The range scanned for each enum. Every enum below numbers from 0 with no gaps, but the
/// scan does not stop at the first hole: a missing case would otherwise truncate the walk
/// and hide every value after it.
inline constexpr int kScanLimit = 64;

/// Calls `visit(value, spelling)` for every enumerator of E.
template <class E, class Visit>
void for_each(Visit&& visit)
{
    for (int value = 0; value < kScanLimit; ++value) {
        if (const char* name = spell(static_cast<E>(value)); name != nullptr) {
            visit(static_cast<E>(value), name);
        }
    }
}

} // namespace ta_roienc_enum_table
