// Every enumerator of every enum the Python bindings expose -- one half of a surface check.
//
// §58.3 asks the wrapper to bind the library's vocabulary, and an enum is the part of that
// vocabulary a binding can lose silently: a value added in C++ compiles, links, passes every
// C++ test and simply does not exist in Python. Slice Z8 found five such holes in structs by
// reading them side by side; the enums were read the same way and three holes survived it --
// the native oneVPL backend, and two status codes a caught exception could already carry.
// Reading does not scale, so this prints what C++ has and integration/scripts/
// python-enum-parity.py checks that Python has exactly that.
//
// **How it stays honest as the library grows.** Each enum is spelled out in a switch with no
// `default:`, so adding an enumerator makes the compiler warn here (-Wswitch, in -Wall) --
// and if that warning is missed, the value never reaches the dump and the parity step fails
// with the value named. Both ends have to be ignored for a hole to open.
//
// The spelling printed is the C++ enumerator, not the display name: `backend_name` says
// "qsv" for IntelOneVpl, and what is being compared here is the surface, not the wording.

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/frame_buffer.h"
#include "sve/logging.h"
#include "sve/object_priority.h"
#include "sve/policy.h"
#include "sve/status.h"
#include "sve/types.h"

#include <cstdio>

namespace {

/// The range scanned for each enum. Every enum below numbers from 0 with no gaps, but the
/// scan does not stop at the first hole: a missing case would otherwise truncate the dump
/// and report every later value as missing from C++, which is the opposite of the truth.
constexpr int kScanLimit = 64;

const char* spell(sve::Backend e)
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

const char* spell(sve::StatusCode e)
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

const char* spell(sve::PixelFormat e)
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

const char* spell(sve::Codec e)
{
    switch (e) {
    case sve::Codec::H264:         return "H264";
    case sve::Codec::H265:         return "H265";
    case sve::Codec::SameAsSource: return "SameAsSource";
    }
    return nullptr;
}

const char* spell(sve::LogLevel e)
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

const char* spell(sve::ObjectPriority e)
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

const char* spell(sve::EncoderState e)
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

const char* spell(sve::SidecarFormat e)
{
    switch (e) {
    case sve::SidecarFormat::None:   return "None";
    case sve::SidecarFormat::NDJSON: return "NDJSON";
    }
    return nullptr;
}

template <class E>
void dump(const char* key)
{
    for (int value = 0; value < kScanLimit; ++value) {
        if (const char* name = spell(static_cast<E>(value)); name != nullptr) {
            std::printf("%s %d %s\n", key, value, name);
        }
    }
}

} // namespace

int main()
{
    dump<sve::Backend>("Backend");
    dump<sve::Codec>("Codec");
    dump<sve::EncoderState>("EncoderState");
    dump<sve::LogLevel>("LogLevel");
    dump<sve::ObjectPriority>("ObjectPriority");
    dump<sve::PixelFormat>("PixelFormat");
    dump<sve::SidecarFormat>("SidecarFormat");
    dump<sve::StatusCode>("StatusCode");
    return 0;
}
