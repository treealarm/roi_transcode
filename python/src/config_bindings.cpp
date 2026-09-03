// Enums, formats and configuration -- TZ v4.2 §58.3.
//
// Everything here is a plain data type on the C++ side, so the bindings are declarations
// rather than logic. That is the shape §58.1 asks for: the Python layer converts types,
// lifetimes and exceptions, and encoding, ROI policy, temporal state and backend fallback
// stay in the core.

#include "bindings.h"

#include <pybind11/functional.h>

#include <cstdio>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace sve_python {

namespace {

/// Stops a Python callable from unwinding into the library.
///
/// The C++ contract for a LogSink says it must not throw: an exception escaping it travels
/// through code that has a frame in flight, and there is nothing sensible to do with it
/// there. In Python raising is ordinary, so a sink that raises is a matter of when rather
/// than whether. The exception stops here and is reported the one way that cannot recurse
/// into the sink that just failed -- on stderr.
///
/// `sink` arrives already converted by pybind11, which is what makes this safe: its holder
/// takes the GIL both to call the callable and to release it, so this wrapper may be copied
/// into the encoder and destroyed on whichever thread gets there last.
sve::LogSink guarded(sve::LogSink sink, const char* what)
{
    if (!sink) {
        return {};
    }
    return [sink = std::move(sink), what](sve::LogLevel level, std::string_view message) {
        try {
            sink(level, message);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sve: %s raised: %s\n", what, e.what());
        } catch (...) {
            std::fprintf(stderr, "sve: %s raised\n", what);
        }
    };
}

} // namespace

void register_config(py::module_& m)
{
    py::enum_<sve::Codec>(m, "Codec", "Output codec (§8.2).")
        .value("H264", sve::Codec::H264)
        .value("H265", sve::Codec::H265)
        .value("SAME_AS_SOURCE", sve::Codec::SameAsSource);

    // The §8.2 spelling, which is also what §58.3's example uses.
    py::enum_<sve::Backend>(m, "Backend", "Which backend encodes (§8.2, §8.4).")
        .value("AUTO", sve::Backend::Auto)
        .value("INTEL_ONE_VPL", sve::Backend::IntelOneVpl)
        .value("VAAPI", sve::Backend::Vaapi)
        .value("NVIDIA_NVENC", sve::Backend::NvidiaNvenc)
        .value("X264", sve::Backend::X264)
        .value("X265", sve::Backend::X265)
        .value("X264_DIRECT", sve::Backend::X264Direct)
        .value("X265_DIRECT", sve::Backend::X265Direct)
        .value("INTEL_ONE_VPL_DIRECT", sve::Backend::IntelOneVplDirect);

    py::enum_<sve::PixelFormat>(m, "PixelFormat", "Input pixel layouts (§8.3, §17).")
        // Bound although nobody would ask for it: it is the value a format field holds
        // before anyone sets one, and an enum that cannot spell what a field contains
        // hands the reader an int with no name on it.
        .value("UNKNOWN", sve::PixelFormat::Unknown)
        .value("YUV420P", sve::PixelFormat::YUV420P)
        .value("NV12", sve::PixelFormat::NV12)
        .value("YUV422P", sve::PixelFormat::YUV422P)
        .value("YUV444P", sve::PixelFormat::YUV444P)
        .value("GRAY8", sve::PixelFormat::GRAY8)
        .value("P010", sve::PixelFormat::P010)
        .value("HARDWARE_SURFACE", sve::PixelFormat::HardwareSurface);

    py::enum_<sve::StatusCode>(m, "StatusCode", "§25's error model, carried by exceptions.")
        .value("OK", sve::StatusCode::Ok)
        .value("DEVICE_UNAVAILABLE", sve::StatusCode::DeviceUnavailable)
        .value("UNSUPPORTED", sve::StatusCode::Unsupported)
        .value("INVALID_ARGUMENT", sve::StatusCode::InvalidArgument)
        .value("AGAIN", sve::StatusCode::Again)
        .value("END_OF_STREAM", sve::StatusCode::EndOfStream)
        .value("BACKEND_ERROR", sve::StatusCode::BackendError)
        .value("OUT_OF_MEMORY", sve::StatusCode::OutOfMemory)
        // Neither is produced by the normative API -- Timeout only by §24's optional
        // blocking variants, Error by whatever has no better name -- but a code that can
        // reach a caller as `status_code` and has no spelling in the enum is a number the
        // caller cannot compare against anything.
        .value("TIMEOUT", sve::StatusCode::Timeout)
        .value("ERROR", sve::StatusCode::Error);

    py::enum_<sve::ObjectPriority>(m, "ObjectPriority", "§11.1's priority ladder.")
        .value("BACKGROUND", sve::ObjectPriority::Background)
        .value("LOW", sve::ObjectPriority::Low)
        .value("NORMAL", sve::ObjectPriority::Normal)
        .value("HIGH", sve::ObjectPriority::High)
        .value("CRITICAL", sve::ObjectPriority::Critical);

    py::enum_<sve::EncoderState>(m, "EncoderState", "§8.13's lifecycle.")
        .value("CREATED", sve::EncoderState::Created)
        .value("CONFIGURED", sve::EncoderState::Configured)
        .value("RUNNING", sve::EncoderState::Running)
        .value("DRAINING", sve::EncoderState::Draining)
        .value("END_OF_STREAM", sve::EncoderState::EndOfStream)
        .value("STOPPED", sve::EncoderState::Stopped)
        .value("FAILED", sve::EncoderState::Failed);

    // §29's five levels. Bound because EncoderConfig.log hands one to the sink, and a
    // sink that received an opaque value it could not name would not be much of a channel.
    py::enum_<sve::LogLevel>(m, "LogLevel", "§29's severity levels.")
        .value("TRACE", sve::LogLevel::Trace)
        .value("DEBUG", sve::LogLevel::Debug)
        .value("INFO", sve::LogLevel::Info)
        .value("WARNING", sve::LogLevel::Warning)
        .value("ERROR", sve::LogLevel::Error);
    m.def("log_level_name", &sve::log_level_name, py::arg("level"),
        "The level's own name, the same spelling the C++ side prints (§29).");

    py::enum_<sve::SidecarFormat>(m, "SidecarFormat", "§19.3's sidecar channel.")
        .value("NONE", sve::SidecarFormat::None)
        .value("NDJSON", sve::SidecarFormat::NDJSON);

    py::class_<sve::Rational>(m, "Rational", "An exact ratio -- frame rate or time base.")
        .def(py::init<>())
        .def(py::init([](int32_t num, int32_t den) { return sve::Rational{num, den}; }),
            py::arg("num"), py::arg("den"))
        .def_readwrite("num", &sve::Rational::num)
        .def_readwrite("den", &sve::Rational::den)
        .def("__repr__", [](const sve::Rational& r) {
            return "Rational(" + std::to_string(r.num) + ", " + std::to_string(r.den) + ")";
        });

    py::class_<sve::InputVideoFormat>(m, "InputVideoFormat", "§8.3's input description.")
        .def(py::init<>())
        .def_readwrite("width", &sve::InputVideoFormat::width)
        .def_readwrite("height", &sve::InputVideoFormat::height)
        .def_readwrite("pixel_format", &sve::InputVideoFormat::pixel_format)
        .def_readwrite("frame_rate", &sve::InputVideoFormat::frame_rate)
        .def_readwrite("time_base", &sve::InputVideoFormat::time_base)
        .def("valid", &sve::InputVideoFormat::valid);

    py::class_<sve::BackendSelection>(m, "BackendSelection",
        "§8.4: which backends may be tried, in order. There is no default order.")
        .def(py::init<>())
        .def_readwrite("preferred", &sve::BackendSelection::preferred)
        .def_readwrite("allow_unlisted", &sve::BackendSelection::allow_unlisted);

    py::class_<sve::MetadataConfig>(m, "MetadataConfig", "§19: where object metadata goes.")
        .def(py::init<>())
        .def_readwrite("bitstream_sei", &sve::MetadataConfig::bitstream_sei)
        .def_readwrite("max_objects", &sve::MetadataConfig::max_objects)
        .def_readwrite("sidecar", &sve::MetadataConfig::sidecar)
        // Guarded for the same reason as EncoderConfig.log, and it was not before: this one
        // is called once per emitted packet, so a raising sink here unwinds through the
        // encoder with a frame in flight rather than merely at open.
        .def_property("sidecar_sink",
            [](const sve::MetadataConfig& cfg) { return cfg.sidecar_sink; },
            [](sve::MetadataConfig& cfg, sve::SidecarSink sink) {
                if (!sink) {
                    cfg.sidecar_sink = {};
                    return;
                }
                cfg.sidecar_sink = [sink = std::move(sink)](std::string_view record) {
                    try {
                        sink(record);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "sve: sidecar sink raised: %s\n", e.what());
                    } catch (...) {
                        std::fprintf(stderr, "sve: sidecar sink raised\n");
                    }
                };
            });

    py::class_<sve::TemporalROIConfig>(m, "TemporalROIConfig", "§13's anti-flicker.")
        .def(py::init<>())
        // The three that were missing until 2026-09-02. `enabled` is §13's own switch and
        // was added to the C++ struct the same day this gap was found -- a Python caller
        // could not turn the section off at all. The other two are the patience: how many
        // of the producer's frames a region survives unseen, and the seconds that caps it.
        // Without them the anti-flicker was configurable in name only.
        .def_readwrite("enabled", &sve::TemporalROIConfig::enabled)
        .def_readwrite("max_missing_frames", &sve::TemporalROIConfig::max_missing_frames)
        .def_readwrite("max_hold_sec", &sve::TemporalROIConfig::max_hold_sec)
        .def_readwrite("confidence_decay", &sve::TemporalROIConfig::confidence_decay)
        .def_readwrite("spatial_smoothing", &sve::TemporalROIConfig::spatial_smoothing)
        .def_readwrite("temporal_smoothing", &sve::TemporalROIConfig::temporal_smoothing)
        .def_readwrite("max_qp_delta_per_frame",
            &sve::TemporalROIConfig::max_qp_delta_per_frame)
        .def_readwrite("use_track_prediction", &sve::TemporalROIConfig::use_track_prediction);

    py::class_<sve::RateControlPolicy>(m, "RateControl", "§15's bitrate policy.")
        .def(py::init<>())
        .def_readwrite("target_bitrate", &sve::RateControlPolicy::target_bitrate)
        .def_readwrite("max_average_overshoot",
            &sve::RateControlPolicy::max_average_overshoot)
        .def_readwrite("max_peak_overshoot", &sve::RateControlPolicy::max_peak_overshoot)
        .def_readwrite("recovery_window_ms", &sve::RateControlPolicy::recovery_window_ms);

    py::class_<sve::KindQuality>(m, "KindQuality",
        "What one class of object is worth: its quantiser offset and its halo (§11).")
        .def(py::init<>())
        .def_readwrite("qp_delta", &sve::KindQuality::qp_delta)
        .def_readwrite("pad", &sve::KindQuality::pad);

    py::class_<sve::Quality>(m, "Quality",
        "The ROI policy of §11: how much the background gives up, how far the halo reaches, "
        "and what each kind of object is worth. These are the knobs the CLI cannot reach, "
        "which is half the reason §58 exists.")
        .def(py::init<>())
        .def_readwrite("background_qp_delta", &sve::Quality::background_qp_delta)
        .def_readwrite("hold_sec", &sve::Quality::hold_sec)
        .def_readwrite("max_regions", &sve::Quality::max_regions)
        .def_readwrite("kinds", &sve::Quality::kinds)
        .def_readwrite("pad", &sve::Quality::pad)
        .def_readwrite("margin_px", &sve::Quality::margin_px)
        .def_readwrite("min_roi_side", &sve::Quality::min_roi_side)
        .def_readwrite("mask_qp_delta_at_255", &sve::Quality::mask_qp_delta_at_255)
        .def_readwrite("mask_levels", &sve::Quality::mask_levels)
        .def_readwrite("max_protected_fraction", &sve::Quality::max_protected_fraction);

    py::class_<sve::Settings>(m, "Settings",
        "The adaptive layer's knobs -- the AdaptiveConfig of §58.3, under the name the C++ "
        "API gives it.")
        .def(py::init<>())
        .def_readwrite("encoder", &sve::Settings::encoder)
        .def_readwrite("backend_selection", &sve::Settings::backend_selection)
        .def_readwrite("codec", &sve::Settings::codec)
        .def_readwrite("crf", &sve::Settings::crf)
        .def_readwrite("gop", &sve::Settings::gop)
        .def_readwrite("quality", &sve::Settings::quality)
        .def_readwrite("temporal", &sve::Settings::temporal)
        .def_readwrite("metadata", &sve::Settings::metadata)
        .def_readwrite("rate_control", &sve::Settings::rate_control);

    py::class_<sve::EncoderConfig>(m, "EncoderConfig", "§8.2's public configuration.")
        .def(py::init<>())
        .def_readwrite("input", &sve::EncoderConfig::input)
        .def_readwrite("settings", &sve::EncoderConfig::settings)
        // §29's log sink, absent from the binding until 2026-09-02: a Python application
        // could not send the library's diagnostics anywhere of its own choosing, which is
        // the one thing sve/logging.h exists to allow. Left unset they go to stderr, as
        // they do in C++.
        //
        // A property rather than def_readwrite because of the guard below. pybind11 does
        // the hard half: the callable it hands us is GIL-safe both to call from a thread
        // the encoder made and to destroy, which is what §58.5 asks for.
        .def_property("log",
            [](const sve::EncoderConfig& cfg) { return cfg.log; },
            [](sve::EncoderConfig& cfg, sve::LogSink sink) {
                cfg.log = guarded(std::move(sink), "log sink");
            });

    py::class_<sve::EncoderCapabilities>(m, "EncoderCapabilities", "§7.2, §23.")
        .def(py::init<>())
        .def_readonly("h264", &sve::EncoderCapabilities::h264)
        .def_readonly("h265", &sve::EncoderCapabilities::h265)
        .def_readonly("roi_rectangles", &sve::EncoderCapabilities::roi_rectangles)
        .def_readonly("qp_map", &sve::EncoderCapabilities::qp_map)
        .def_readonly("qp_map_block_width", &sve::EncoderCapabilities::qp_map_block_width)
        .def_readonly("qp_map_block_height", &sve::EncoderCapabilities::qp_map_block_height)
        .def_readonly("ctu_qp", &sve::EncoderCapabilities::ctu_qp)
        .def_readonly("emphasis_map", &sve::EncoderCapabilities::emphasis_map)
        .def_readonly("runtime_roi_update", &sve::EncoderCapabilities::runtime_roi_update)
        .def_readonly("sei", &sve::EncoderCapabilities::sei)
        .def_readonly("native_bitrate", &sve::EncoderCapabilities::native_bitrate)
        .def_readonly("main10", &sve::EncoderCapabilities::main10)
        .def_readonly("p010_input", &sve::EncoderCapabilities::p010_input)
        .def_readonly("max_roi_regions", &sve::EncoderCapabilities::max_roi_regions);

    py::class_<sve::EncoderStatistics>(m, "EncoderStatistics", "§21, §22, §28.")
        .def(py::init<>())
        .def_readonly("frames_submitted", &sve::EncoderStatistics::frames_submitted)
        .def_readonly("packets_received", &sve::EncoderStatistics::packets_received)
        .def_readonly("bytes_encoded", &sve::EncoderStatistics::bytes_encoded)
        .def_readonly("frames_converted", &sve::EncoderStatistics::frames_converted)
        .def_readonly("frames_uploaded", &sve::EncoderStatistics::frames_uploaded)
        .def_readonly("regions_dropped", &sve::EncoderStatistics::regions_dropped)
        .def_readonly("backends_declined", &sve::EncoderStatistics::backends_declined)
        .def_readonly("fallbacks_applied", &sve::EncoderStatistics::fallbacks_applied)
        .def_readonly("encoder_used", &sve::EncoderStatistics::encoder_used)
        // Everything below was absent until 2026-09-02, and the first three are why it
        // mattered. §50 forbids a silent degradation, and these are how two of them become
        // visible: a quality map that ran off the end of the quantiser scale, and a target
        // bitrate refused on CPU H.265. A Python caller could read neither, so §50 was
        // satisfied in C++ and not in the binding. `backend_name` is the prose name of what
        // opened, which under Backend::AUTO is the only way to find out.
        .def_readonly("frames_quantiser_clamped",
            &sve::EncoderStatistics::frames_quantiser_clamped)
        .def_readonly("cpu_h265_target_guard",
            &sve::EncoderStatistics::cpu_h265_target_guard)
        .def_readonly("bitrate_target_unreachable",
            &sve::EncoderStatistics::bitrate_target_unreachable)
        .def_readonly("keyframes_requested", &sve::EncoderStatistics::keyframes_requested)
        .def_readonly("backend_name", &sve::EncoderStatistics::backend_name)
        // §14's ROI lag, reported when the boxes carried track ids to measure it from.
        .def_readonly("roi_lag_measured", &sve::EncoderStatistics::roi_lag_measured)
        .def_readonly("roi_lag_samples", &sve::EncoderStatistics::roi_lag_samples)
        .def_readonly("roi_lag_min_iou", &sve::EncoderStatistics::roi_lag_min_iou)
        .def_readonly("roi_lag_p95_frames", &sve::EncoderStatistics::roi_lag_p95_frames)
        .def_readonly("roi_lag_p99_frames", &sve::EncoderStatistics::roi_lag_p99_frames)
        .def_readonly("roi_lag_p95_ms", &sve::EncoderStatistics::roi_lag_p95_ms)
        .def_readonly("roi_lag_p99_ms", &sve::EncoderStatistics::roi_lag_p99_ms)
        .def_readonly("roi_lag_acceptance", &sve::EncoderStatistics::roi_lag_acceptance);

    // §38's other half of the capability story: what the encoder settled on, not what it
    // could have done. Optional fields, and None means "this backend cannot say" -- which
    // is not the same as the feature being off.
    py::class_<sve::EffectiveSettings>(m, "EffectiveSettings",
        "What the opened encoder actually configured (§15.1, §38).")
        .def(py::init<>())
        .def_readonly("b_frames", &sve::EffectiveSettings::b_frames)
        .def_readonly("lookahead_frames", &sve::EffectiveSettings::lookahead_frames)
        .def_readonly("spatial_aq", &sve::EffectiveSettings::spatial_aq)
        .def_readonly("temporal_aq", &sve::EffectiveSettings::temporal_aq);
}

} // namespace sve_python
