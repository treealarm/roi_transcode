#pragma once

// Every enumerator of every enum the public API exposes, spelled out once.
//
// labels_test asks the library's own *_name() function for each value; keeping the list here
// rather than inside the test is what lets a new enum be added in one obvious place.
//
// Each enum is a switch with no `default:`, so adding an enumerator makes the compiler warn
// here (-Wswitch, in -Wall). That warning is the first line of defence; labels_test is the
// second.

#include "ta_roienc/engine_order.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/picture_store.h"
#include "ta_roienc/log.h"
#include "ta_roienc/salience.h"
#include "ta_roienc/rules.h"
#include "ta_roienc/verdict.h"
#include "ta_roienc/basics.h"

namespace ta_roienc_enum_table {

inline const char* spell(ta_roienc::Engine e)
{
    switch (e) {
    case ta_roienc::Engine::Auto:              return "Auto";
    case ta_roienc::Engine::IntelOneVpl:       return "IntelOneVpl";
    case ta_roienc::Engine::Vaapi:             return "Vaapi";
    case ta_roienc::Engine::X264:              return "X264";
    case ta_roienc::Engine::X265:              return "X265";
    case ta_roienc::Engine::X264Direct:        return "X264Direct";
    case ta_roienc::Engine::X265Direct:        return "X265Direct";
    case ta_roienc::Engine::IntelOneVplDirect: return "IntelOneVplDirect";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::VerdictCode e)
{
    switch (e) {
    case ta_roienc::VerdictCode::Ok:                return "Ok";
    case ta_roienc::VerdictCode::DeviceUnavailable: return "DeviceUnavailable";
    case ta_roienc::VerdictCode::Unsupported:       return "Unsupported";
    case ta_roienc::VerdictCode::InvalidArgument:   return "InvalidArgument";
    case ta_roienc::VerdictCode::Again:             return "Again";
    case ta_roienc::VerdictCode::EndOfStream:       return "EndOfStream";
    case ta_roienc::VerdictCode::BackendError:      return "BackendError";
    case ta_roienc::VerdictCode::OutOfMemory:       return "OutOfMemory";
    case ta_roienc::VerdictCode::Timeout:           return "Timeout";
    case ta_roienc::VerdictCode::Error:             return "Error";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::PixelLayout e)
{
    switch (e) {
    case ta_roienc::PixelLayout::Unknown:         return "Unknown";
    case ta_roienc::PixelLayout::YUV420P:         return "YUV420P";
    case ta_roienc::PixelLayout::NV12:            return "NV12";
    case ta_roienc::PixelLayout::YUV422P:         return "YUV422P";
    case ta_roienc::PixelLayout::YUV444P:         return "YUV444P";
    case ta_roienc::PixelLayout::GRAY8:           return "GRAY8";
    case ta_roienc::PixelLayout::P010:            return "P010";
    case ta_roienc::PixelLayout::YUV420P10:       return "YUV420P10";
    case ta_roienc::PixelLayout::HardwareSurface: return "HardwareSurface";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::Coding e)
{
    switch (e) {
    case ta_roienc::Coding::H264:         return "H264";
    case ta_roienc::Coding::H265:         return "H265";
    case ta_roienc::Coding::SameAsSource: return "SameAsSource";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::Verbosity e)
{
    switch (e) {
    case ta_roienc::Verbosity::Trace:   return "Trace";
    case ta_roienc::Verbosity::Debug:   return "Debug";
    case ta_roienc::Verbosity::Info:    return "Info";
    case ta_roienc::Verbosity::Warning: return "Warning";
    case ta_roienc::Verbosity::Error:   return "Error";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::Salience e)
{
    switch (e) {
    case ta_roienc::Salience::Background: return "Background";
    case ta_roienc::Salience::Low:        return "Low";
    case ta_roienc::Salience::Normal:     return "Normal";
    case ta_roienc::Salience::High:       return "High";
    case ta_roienc::Salience::Critical:   return "Critical";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::Lifecycle e)
{
    switch (e) {
    case ta_roienc::Lifecycle::Created:     return "Created";
    case ta_roienc::Lifecycle::Configured:  return "Configured";
    case ta_roienc::Lifecycle::Running:     return "Running";
    case ta_roienc::Lifecycle::Draining:    return "Draining";
    case ta_roienc::Lifecycle::EndOfStream: return "EndOfStream";
    case ta_roienc::Lifecycle::Stopped:     return "Stopped";
    case ta_roienc::Lifecycle::Failed:      return "Failed";
    }
    return nullptr;
}

inline const char* spell(ta_roienc::MetadataFile e)
{
    switch (e) {
    case ta_roienc::MetadataFile::None:   return "None";
    case ta_roienc::MetadataFile::NDJSON: return "NDJSON";
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
