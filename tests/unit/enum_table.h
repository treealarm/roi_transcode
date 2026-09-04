#pragma once

// Every enumerator of every enum the public API exposes, spelled out once.
//
// Two things need this list and they must not keep separate copies: enum_surface, which
// prints it for the Python surface check, and names_test, which asks the library's own
// *_name() function for each value. A second copy is how one of them ends up describing an
// enum that has moved on.
//
// Each enum is a switch with no `default:`, so adding an enumerator makes the compiler warn
// here (-Wswitch, in -Wall). That warning is the first line of defence; the two consumers
// are the second.

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/frame_buffer.h"
#include "sve/logging.h"
#include "sve/object_priority.h"
#include "sve/policy.h"
#include "sve/status.h"
#include "sve/types.h"

namespace sve_enum_table {

inline const char* spell(sve::Backend e)
{
    switch (e) {
    case sve::Backend::Auto:              return "Auto";
    case sve::Backend::IntelOneVpl:       return "IntelOneVpl";
    case sve::Backend::Vaapi:             return "Vaapi";
    case sve::Backend::NvidiaNvenc:       return "NvidiaNvenc";
    case sve::Backend::X264:              return "X264";
    case sve::Backend::X265:              return "X265";
    case sve::Backend::X264Direct:        return "X264Direct";
    case sve::Backend::X265Direct:        return "X265Direct";
    case sve::Backend::IntelOneVplDirect: return "IntelOneVplDirect";
    }
    return nullptr;
}

inline const char* spell(sve::StatusCode e)
{
    switch (e) {
    case sve::StatusCode::Ok:                return "Ok";
    case sve::StatusCode::DeviceUnavailable: return "DeviceUnavailable";
    case sve::StatusCode::Unsupported:       return "Unsupported";
    case sve::StatusCode::InvalidArgument:   return "InvalidArgument";
    case sve::StatusCode::Again:             return "Again";
    case sve::StatusCode::EndOfStream:       return "EndOfStream";
    case sve::StatusCode::BackendError:      return "BackendError";
    case sve::StatusCode::OutOfMemory:       return "OutOfMemory";
    case sve::StatusCode::Timeout:           return "Timeout";
    case sve::StatusCode::Error:             return "Error";
    }
    return nullptr;
}

inline const char* spell(sve::PixelFormat e)
{
    switch (e) {
    case sve::PixelFormat::Unknown:         return "Unknown";
    case sve::PixelFormat::YUV420P:         return "YUV420P";
    case sve::PixelFormat::NV12:            return "NV12";
    case sve::PixelFormat::YUV422P:         return "YUV422P";
    case sve::PixelFormat::YUV444P:         return "YUV444P";
    case sve::PixelFormat::GRAY8:           return "GRAY8";
    case sve::PixelFormat::P010:            return "P010";
    case sve::PixelFormat::HardwareSurface: return "HardwareSurface";
    }
    return nullptr;
}

inline const char* spell(sve::Codec e)
{
    switch (e) {
    case sve::Codec::H264:         return "H264";
    case sve::Codec::H265:         return "H265";
    case sve::Codec::SameAsSource: return "SameAsSource";
    }
    return nullptr;
}

inline const char* spell(sve::LogLevel e)
{
    switch (e) {
    case sve::LogLevel::Trace:   return "Trace";
    case sve::LogLevel::Debug:   return "Debug";
    case sve::LogLevel::Info:    return "Info";
    case sve::LogLevel::Warning: return "Warning";
    case sve::LogLevel::Error:   return "Error";
    }
    return nullptr;
}

inline const char* spell(sve::ObjectPriority e)
{
    switch (e) {
    case sve::ObjectPriority::Background: return "Background";
    case sve::ObjectPriority::Low:        return "Low";
    case sve::ObjectPriority::Normal:     return "Normal";
    case sve::ObjectPriority::High:       return "High";
    case sve::ObjectPriority::Critical:   return "Critical";
    }
    return nullptr;
}

inline const char* spell(sve::EncoderState e)
{
    switch (e) {
    case sve::EncoderState::Created:     return "Created";
    case sve::EncoderState::Configured:  return "Configured";
    case sve::EncoderState::Running:     return "Running";
    case sve::EncoderState::Draining:    return "Draining";
    case sve::EncoderState::EndOfStream: return "EndOfStream";
    case sve::EncoderState::Stopped:     return "Stopped";
    case sve::EncoderState::Failed:      return "Failed";
    }
    return nullptr;
}

inline const char* spell(sve::SidecarFormat e)
{
    switch (e) {
    case sve::SidecarFormat::None:   return "None";
    case sve::SidecarFormat::NDJSON: return "NDJSON";
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

} // namespace sve_enum_table
