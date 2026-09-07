#include "decode_device_access.h"

#include "av_raii.h"

#include <cstdlib>
#include <format>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace sve {

struct DecodeDevice::Impl {
    DecodeBackend kind = DecodeBackend::Cpu;
    roiav::Buffer device;
    std::vector<std::string> skipped;
};

namespace {

AVHWDeviceType hw_type_of(DecodeBackend backend) noexcept
{
    switch (backend) {
    case DecodeBackend::Vaapi:
        return AV_HWDEVICE_TYPE_VAAPI;
    case DecodeBackend::IntelQsv:
        return AV_HWDEVICE_TYPE_QSV;
    case DecodeBackend::NvidiaNvdec:
        return AV_HWDEVICE_TYPE_CUDA;
    case DecodeBackend::Auto:
    case DecodeBackend::Cpu:
        return AV_HWDEVICE_TYPE_NONE;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

/// Whether this build of FFmpeg contains the device type at all.
///
/// Asked rather than assumed, because the answer differs between our own images: this
/// tree's FFmpeg has VA-API and QSV and no CUDA, and a backend that is simply absent must
/// say so as itself rather than as "the device would not open" -- the two have different
/// fixes, one in the image and one on the machine.
bool device_type_present(AVHWDeviceType type) noexcept
{
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         t != AV_HWDEVICE_TYPE_NONE; t = av_hwdevice_iterate_types(t)) {
        if (t == type) {
            return true;
        }
    }
    return false;
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

DecodeBackend DecodeDevice::kind() const noexcept
{
    return m_impl ? m_impl->kind : DecodeBackend::Cpu;
}

std::string_view DecodeDevice::name() const noexcept
{
    return decode_backend_name(kind());
}

std::span<const std::string> DecodeDevice::skipped() const noexcept
{
    return m_impl ? std::span<const std::string>(m_impl->skipped) : std::span<const std::string>{};
}

Result<DecodeDevice> DecodeDevice::Open(const DecodeSelection& selection)
{
    if (selection.preferred.empty()) {
        return Status::InvalidArgument(
            "no decode backend was listed; naming the order is the caller's to do");
    }

    auto impl = std::make_shared<Impl>();
    for (const DecodeBackend backend : selection.preferred) {
        if (backend == DecodeBackend::Auto) {
            impl->skipped.emplace_back("auto: not a backend, it is the list itself");
            continue;
        }
        if (backend == DecodeBackend::Cpu) {
            // Reached only when every hardware entry ahead of it declined, which is the
            // ordinary case on a machine without a GPU. The CPU device holds nothing, so
            // what is returned is the default-constructed handle -- but it carries the
            // record of what was tried, which is the part worth keeping.
            impl->kind = DecodeBackend::Cpu;
            DecodeDevice device;
            device.m_impl = std::move(impl);
            return device;
        }

        const AVHWDeviceType type = hw_type_of(backend);
        if (!device_type_present(type)) {
            impl->skipped.emplace_back(std::format(
                "{}: not built into this FFmpeg", decode_backend_name(backend)));
            continue;
        }
        // QSV reaches the same Intel silicon through libva on Linux, so it needs the driver
        // path as much as VA-API does; only CUDA does not.
        if (backend == DecodeBackend::Vaapi || backend == DecodeBackend::IntelQsv) {
            ensure_libva_driver_path();
        }

        // Two attempts, and the second is not redundant: letting libva choose (a null path)
        // is right on a machine with one GPU and has been seen to fail outright on machines
        // where it is not, so the render node is named explicitly as a fallback. The error
        // from the first attempt is what gets reported if both fail, because that is the one
        // describing the configuration the caller actually asked for.
        roiav::Buffer opened;
        int err = av_hwdevice_ctx_create(&opened.p, type, nullptr, nullptr, 0);
        if (err < 0 && type != AV_HWDEVICE_TYPE_CUDA) {
            roiav::Buffer named;
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
    return Status::DeviceUnavailable(
        reasons.empty() ? "no decode device opened" : "no decode device opened: " + reasons);
}

} // namespace sve
