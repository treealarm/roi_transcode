#include "decode_hardware_access.h"

#include "av_handles.h"
#include "engine/common/av_hw_device.h"

#include <cstdlib>
#include <format>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace ta_roienc {

struct DecodeDevice::Internals {
    DecodeEngine kind = DecodeEngine::Cpu;
    roiav::ByteBlock device;
    std::vector<std::string> skipped;
};

namespace {

AVHWDeviceType hw_type_of(DecodeEngine backend) noexcept
{
    switch (backend) {
    case DecodeEngine::Vaapi:
        return AV_HWDEVICE_TYPE_VAAPI;
    case DecodeEngine::IntelQsv:
        return AV_HWDEVICE_TYPE_QSV;
    case DecodeEngine::NvidiaNvdec:
        return AV_HWDEVICE_TYPE_CUDA;
    case DecodeEngine::D3d11va:
        return AV_HWDEVICE_TYPE_D3D11VA;
    case DecodeEngine::Auto:
    case DecodeEngine::Cpu:
        return AV_HWDEVICE_TYPE_NONE;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

/// vcpkg's libva compiles in its own build prefix as the driver search path, and that
/// directory does not exist in any image we ship. Without this, va_openDriver() looks
/// there, returns -1, and the only thing said out loud is "failed to initialise VAAPI
/// connection" -- which reads as missing hardware rather than a missing path.
///
/// Set, never overridden: a deployment that knows better has already said so.
void ensure_libva_driver_path()
{
#if !defined(_WIN32)
    ::setenv("LIBVA_DRIVERS_PATH", "/usr/lib/x86_64-linux-gnu/dri", 0);
#endif
}

} // namespace

AVBufferRef* DecodeDeviceAccess::device(const DecodeDevice& d) noexcept
{
    return d.m_impl ? d.m_impl->device.p : nullptr;
}

DecodeEngine DecodeDevice::kind() const noexcept
{
    return m_impl ? m_impl->kind : DecodeEngine::Cpu;
}

std::string_view DecodeDevice::name() const noexcept
{
    return decode_backend_name(kind());
}

std::span<const std::string> DecodeDevice::skipped() const noexcept
{
    return m_impl ? std::span<const std::string>(m_impl->skipped) : std::span<const std::string>{};
}

Fallible<DecodeDevice> DecodeDevice::Opened(const DecodeOrder& selection)
{
    if (selection.preferred.empty()) {
        return Verdict::InvalidArgument(
            "no decode backend was listed; naming the order is the caller's to do");
    }

    auto impl = std::make_shared<Internals>();
    for (const DecodeEngine backend : selection.preferred) {
        if (backend == DecodeEngine::Auto) {
            impl->skipped.emplace_back("auto: not a backend, it is the list itself");
            continue;
        }
        if (backend == DecodeEngine::Cpu) {
            // Reached only when every hardware entry ahead of it declined, which is the
            // ordinary case on a machine without a GPU. The CPU device holds nothing, so
            // what is returned is the default-constructed handle -- but it carries the
            // record of what was tried, which is the part worth keeping.
            impl->kind = DecodeEngine::Cpu;
            DecodeDevice device;
            device.m_impl = std::move(impl);
            return device;
        }

        const AVHWDeviceType type = hw_type_of(backend);
        // Asked rather than assumed, because the answer differs between our own builds:
        // the Linux images' FFmpeg has VA-API and QSV and no CUDA, the Windows one has
        // D3D11VA and CUDA and neither of the others.
        if (!hw_device_type_present(type)) {
            impl->skipped.emplace_back(std::format(
                "{}: not built into this FFmpeg", decode_backend_name(backend)));
            continue;
        }
        // QSV reaches the same Intel silicon through libva on Linux, so it needs the driver
        // path and the render node as much as VA-API does; CUDA and D3D11VA need neither,
        // and to D3D11VA a device string is an adapter number, not a path.
        const bool through_libva =
            backend == DecodeEngine::Vaapi || backend == DecodeEngine::IntelQsv;
        if (through_libva) {
            ensure_libva_driver_path();
        }

        // Two attempts, and the second is not redundant: letting libva choose (a null path)
        // is right on a machine with one GPU and has been seen to fail outright on machines
        // where it is not, so the render node is named explicitly as a fallback. The error
        // from the first attempt is what gets reported if both fail, because that is the one
        // describing the configuration the caller actually asked for.
        roiav::ByteBlock opened;
        int err = av_hwdevice_ctx_create(&opened.p, type, nullptr, nullptr, 0);
        if (err < 0 && through_libva) {
            roiav::ByteBlock named;
            if (av_hwdevice_ctx_create(&named.p, type, "/dev/dri/renderD128", nullptr, 0) >= 0) {
                opened = std::move(named);
                err = 0;
            }
        }
        if (err < 0) {
            char message[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(err, message, sizeof(message));
            impl->skipped.emplace_back(
                std::format("{}: {}", decode_backend_name(backend), message));
            continue;
        }

        impl->kind = backend;
        impl->device = std::move(opened);
        DecodeDevice device;
        device.m_impl = std::move(impl);
        return device;
    }

    std::string reasons;
    for (const std::string& reason : impl->skipped) {
        if (!reasons.empty()) {
            reasons += "; ";
        }
        reasons += reason;
    }
    return Verdict::DeviceUnavailable(
        reasons.empty() ? "no decode device opened" : "no decode device opened: " + reasons);
}

} // namespace ta_roienc
