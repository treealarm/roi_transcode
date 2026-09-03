// sve-baseline -- the calibration run and the baseline manifest of TZ v4.2 §38.
//
//     sve-baseline --hardware-class NAME --dataset NAME --backend NAME
//                  --codec h264|h265 --width W --height H --fps N --point R1|R2|R3|R4
//
// §38 asks that, before final acceptance, a baseline manifest be created for each
// combination of backend, codec, hardware class, resolution, dataset and operating point,
// and that changing any field create a new profile version. This writes one such manifest
// to stdout, in the shape §38 prints, and the profile version is a fingerprint of the
// manifest's own normative fields -- so "changing any field creates a new version" is
// arithmetic rather than a promise somebody has to keep.
//
// It is a calibration *run*, not a formatter: the encoder is opened and frames are pushed
// through it before anything is written. Two reasons, and both have teeth:
//
//   * A manifest for a configuration this machine cannot produce would be fiction. If the
//     backend refuses to open, nothing is written and the exit code says which kind of
//     refusal it was.
//   * The configuration asked for is not always the configuration that runs. CPU H.265
//     refuses a target bitrate and falls back to CRF (the guard in EncoderStatistics), and
//     a baseline that recorded the request rather than what happened would pin the wrong
//     thing. `rate_control_applied` is read from the encode.
//
// Exit codes are how the driving script tells a refusal from a failure: 0 wrote a manifest,
// 2 is a bad command line, 3 is "this build cannot version this backend" and 4 is "this
// machine cannot open it". 3 and 4 are skips to be counted, not errors -- see §5.5 on why
// only the direct software backends can be versioned today.
//
// What is deliberately NOT here: measured results. §40's curves and §41's statistics are a
// different artefact and they need VMAF, which is deferred (decision Р1 in docs/STATUS.md).
// A manifest is the configuration a later measurement is pinned to.

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/raw_frame.h"
#include "sve/types.h"

#include "sve_build_manifest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

namespace {

/// §39's operating point table. Bitrates in bits per second, by resolution class.
struct OperatingPoints {
    const char* resolution;
    int32_t width;
    int32_t height;
    int32_t fps;
    uint64_t r[4];
};

/// TZ v4.2 §39, transcribed. A resolution outside this table has no operating points
/// defined, and the tool refuses rather than inventing them.
constexpr OperatingPoints kOperatingPoints[] = {
    {"720p30", 1280, 720, 30, {750'000, 1'500'000, 3'000'000, 6'000'000}},
    {"1080p30", 1920, 1080, 30, {1'500'000, 3'000'000, 6'000'000, 12'000'000}},
    {"1080p60", 1920, 1080, 60, {3'000'000, 6'000'000, 12'000'000, 24'000'000}},
    {"4K30", 3840, 2160, 30, {6'000'000, 12'000'000, 24'000'000, 48'000'000}},
};

/// Keyframe interval used for calibration, in seconds. §38's example pins gop_size 60 at
/// 30 fps, which is this; it is set explicitly rather than left to the library's own
/// default so that the manifest describes a decision instead of echoing one.
constexpr int32_t kCalibrationGopSeconds = 2;

/// Seconds of video pushed through before the manifest is written. Two GOPs: enough that
/// rate control has done something and that a keyframe interval has closed twice.
constexpr int32_t kCalibrationSeconds = kCalibrationGopSeconds * 2;

/// FNV-1a, 64-bit -- the published offset basis and prime. Chosen because the fingerprint
/// has to be reproducible by anyone reading this file, and a hash whose constants are a
/// mystery is not. It is an identity, not a security claim.
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fingerprint_of(const std::string& text)
{
    uint64_t h = kFnvOffsetBasis;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime;
    }
    return h;
}

const OperatingPoints* points_for(int32_t width, int32_t height, int32_t fps)
{
    for (const OperatingPoints& p : kOperatingPoints) {
        if (p.width == width && p.height == height && p.fps == fps) {
            return &p;
        }
    }
    return nullptr;
}

/// Which encoding library actually produces the bitstream, and the version of it that runs.
///
/// Only the direct software backends can answer today: they link libx264 and libx265
/// statically, so what pkg-config reported at configure time is what executes. A backend
/// whose encoder lives in a driver -- oneVPL, VA-API, NVENC -- has a runtime version this
/// build cannot see, and §38 asks for the exact one. Writing the build's idea of it would
/// pin a version that may not be the one running, so those are refused instead.
struct EncoderLibrary {
    const char* name;
    const char* version;
};

bool library_for(sve::Backend kind, EncoderLibrary* out)
{
    switch (kind) {
    case sve::Backend::X264Direct:
        *out = {"libx264", sve::build::kX264Version};
        break;
    case sve::Backend::X265Direct:
        *out = {"libx265", sve::build::kX265Version};
        break;
    default:
        return false;
    }
    // An empty version means pkg-config found nothing for it -- the component is not in
    // this build. Pinning a profile to a version string that is not there would be the
    // same fiction as making one up, so it is a refusal rather than an empty field.
    return out->version[0] != '\0';
}

/// A moving edge, so that rate control has something to spend bits on. Flat frames encode
/// to almost nothing and would let a target bitrate look satisfied for the wrong reason.
struct Pixels {
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    Pixels(int32_t width, int32_t height, int64_t n)
        : y(static_cast<std::size_t>(width) * height)
        , u(static_cast<std::size_t>(width) * height / 4, 128)
        , v(static_cast<std::size_t>(width) * height / 4, 128)
    {
        for (int32_t row = 0; row < height; ++row) {
            for (int32_t col = 0; col < width; ++col) {
                const bool lit =
                    ((col + static_cast<int32_t>(n) * 3) / 8 + row / 8) % 2 == 0;
                y[static_cast<std::size_t>(row) * width + col] = lit ? 220 : 40;
            }
        }
    }
};

struct Options {
    std::string hardware_class;
    std::string dataset;
    std::string backend;
    std::string codec = "h264";
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps = 0;
    int point = 0;   // 1..4, from --point R1..R4
};

void usage()
{
    std::fprintf(stderr,
        "Usage: sve-baseline --hardware-class NAME --dataset NAME --backend NAME\n"
        "                    --codec h264|h265 --width W --height H --fps N\n"
        "                    --point R1|R2|R3|R4\n"
        "\n"
        "Writes one TZ v4.2 §38 baseline manifest to stdout, after running the encode it\n"
        "describes. The operating point comes from §39's table for the resolution given;\n"
        "a resolution that table does not list is refused.\n"
        "\n"
        "Exit codes: 0 wrote a manifest, 2 bad command line, 3 this build cannot version\n"
        "this backend's encoder, 4 this machine cannot open it.\n");
}

/// A field the backend could report, or the word that says it could not. §38 pins what ran,
/// and a missing answer written as a default would pin our assumption instead.
std::string number_or_unknown(const std::optional<int32_t>& value)
{
    return value ? std::to_string(*value) : std::string("unknown");
}

std::string flag_or_unknown(const std::optional<bool>& value)
{
    return value ? std::string(*value ? "true" : "false") : std::string("unknown");
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    const auto next = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n", argv[i]);
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--hardware-class") == 0) {
            opt.hardware_class = next(i);
        } else if (std::strcmp(a, "--dataset") == 0) {
            opt.dataset = next(i);
        } else if (std::strcmp(a, "--backend") == 0) {
            opt.backend = next(i);
        } else if (std::strcmp(a, "--codec") == 0) {
            opt.codec = next(i);
        } else if (std::strcmp(a, "--width") == 0) {
            opt.width = std::atoi(next(i));
        } else if (std::strcmp(a, "--height") == 0) {
            opt.height = std::atoi(next(i));
        } else if (std::strcmp(a, "--fps") == 0) {
            opt.fps = std::atoi(next(i));
        } else if (std::strcmp(a, "--point") == 0) {
            const char* p = next(i);
            if (p[0] != 'R' || p[1] < '1' || p[1] > '4' || p[2] != '\0') {
                std::fprintf(stderr, "--point takes R1, R2, R3 or R4, not %s\n", p);
                return 2;
            }
            opt.point = p[1] - '0';
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }

    if (opt.hardware_class.empty() || opt.dataset.empty() || opt.backend.empty()
        || opt.width <= 0 || opt.height <= 0 || opt.fps <= 0 || opt.point == 0) {
        usage();
        return 2;
    }

    const OperatingPoints* points = points_for(opt.width, opt.height, opt.fps);
    if (points == nullptr) {
        std::fprintf(stderr,
            "§39 defines no operating points for %dx%d@%d; the table lists 720p30, "
            "1080p30, 1080p60 and 4K30\n",
            opt.width, opt.height, opt.fps);
        return 2;
    }
    const uint64_t target_bitrate = points->r[opt.point - 1];

    sve::BackendChoice choice;
    try {
        choice = sve::backend_choice_from_name(opt.backend.c_str());
    } catch (const std::invalid_argument&) {
        std::fprintf(stderr, "unknown backend: %s (one of %s)\n", opt.backend.c_str(),
            sve::backend_choice_names().c_str());
        return 2;
    }

    EncoderLibrary library{};
    if (!library_for(choice.kind, &library)) {
        std::fprintf(stderr,
            "%s cannot be calibrated by this build: its encoder's runtime version is not "
            "knowable here, and §38 pins the exact one\n",
            opt.backend.c_str());
        return 3;
    }

    const bool h265 = opt.codec == "h265";
    if (!h265 && opt.codec != "h264") {
        std::fprintf(stderr, "--codec takes h264 or h265, not %s\n", opt.codec.c_str());
        return 2;
    }

    const int32_t gop = kCalibrationGopSeconds * opt.fps;

    sve::EncoderConfig cfg;
    cfg.input.width = opt.width;
    cfg.input.height = opt.height;
    cfg.input.frame_rate = sve::Rational{opt.fps, 1};
    cfg.input.pixel_format = sve::PixelFormat::YUV420P;
    cfg.settings.encoder = choice.kind;
    cfg.settings.backend_selection = choice.selection;
    cfg.settings.codec = h265 ? sve::Codec::H265 : sve::Codec::H264;
    cfg.settings.gop = gop;
    cfg.settings.rate_control.target_bitrate = target_bitrate;
    // Diagnostics would land in the middle of the manifest on stdout. The manifest is the
    // output; what the encoder has to say goes to stderr like every other refusal here.
    cfg.log = [](sve::LogLevel, std::string_view line) {
        std::fprintf(stderr, "encoder: %.*s\n", static_cast<int>(line.size()), line.data());
    };

    std::unique_ptr<sve::SmartVideoEncoder> enc;
    try {
        enc = sve::CreateVideoEncoder(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s cannot open here: %s\n", opt.backend.c_str(), e.what());
        return 4;
    }

    const auto drain = [&] {
        for (;;) {
            sve::EncodedPacket packet;
            const sve::Status s = enc->ReceivePacket(&packet);
            if (s.code() == sve::StatusCode::Again
                || s.code() == sve::StatusCode::EndOfStream) {
                return true;
            }
            if (!s.ok()) {
                std::fprintf(stderr, "receive failed: %s\n", s.message().c_str());
                return false;
            }
        }
    };

    const int64_t frames = static_cast<int64_t>(kCalibrationSeconds) * opt.fps;
    for (int64_t i = 0; i < frames; ++i) {
        auto pixels = std::make_shared<Pixels>(opt.width, opt.height, i);
        sve::RawFrameView view;
        view.format = cfg.input;
        view.plane_count = 3;
        view.planes[0] = {pixels->y.data(), opt.width, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), opt.width / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), opt.width / 2, pixels->v.size()};
        view.timestamp = sve::MediaTimestamp{i, sve::Rational{1, opt.fps}};
        view.owner = pixels;

        sve::TimedFrame timed;
        if (const sve::Status s = sve::WrapRawFrame(view, &timed.frame); !s.ok()) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(i),
                s.message().c_str());
            return 4;
        }
        timed.metadata.timestamp = view.timestamp;
        if (const sve::Status s = enc->SubmitFrame(timed); !s.ok()) {
            std::fprintf(stderr, "submit %lld: %s\n", static_cast<long long>(i),
                s.message().c_str());
            return 4;
        }
        if (!drain()) {
            return 4;
        }
    }
    if (const sve::Status s = enc->Flush(); !s.ok()) {
        std::fprintf(stderr, "flush: %s\n", s.message().c_str());
        return 4;
    }
    if (!drain()) {
        return 4;
    }

    const sve::EncoderStatistics stats = enc->GetStatistics();
    // Asked of the encoder while it is still open: these come from the backend's own
    // session, and after the unique_ptr goes there is nothing left to ask.
    const sve::EffectiveSettings effective = enc->GetEffectiveSettings();
    if (stats.packets_received == 0) {
        std::fprintf(stderr, "%s produced no packets; nothing to pin a baseline to\n",
            opt.backend.c_str());
        return 4;
    }

    // What ran, which is not always what was asked for: CPU H.265 refuses a target bitrate
    // and encodes at CRF instead, and a baseline that recorded the request would pin a rate
    // control this configuration never used.
    const char* rate_control = stats.cpu_h265_target_guard ? "crf" : "vbr";

    // §39: bits per pixel per frame, the dimensionless form the table is compared in.
    const double bpppf = static_cast<double>(target_bitrate)
        / (static_cast<double>(opt.width) * opt.height * opt.fps);

    // The body first, then its fingerprint, then both. Everything that describes the
    // configuration is inside the body and therefore inside the fingerprint; nothing that
    // changes from build to build for the same configuration is -- our own commit is not
    // here for that reason, or a profile would need re-versioning on every commit and the
    // check that guards it would become noise.
    char body[4096];
    const int written = std::snprintf(body, sizeof(body),
        "protocol: SVENC-1.0\n"
        "\n"
        "hardware_class: %s\n"
        "dataset: %s\n"
        "operating_point: R%d\n"
        "\n"
        "backend: %s\n"
        "backend_opened: %s\n"
        "codec: %s\n"
        "\n"
        "encoder:\n"
        "  library: %s\n"
        "  version: \"%s\"\n"
        "  preset: %s\n"
        "  rate_control: %s\n"
        "  target_bitrate: %llu\n"
        "  gop_size: %d\n"
        "  quality_policy: %s\n"
        "  fallback_policy: %s\n"
        "  # The four §15.1 fields, read back from the encoder rather than from the\n"
        "  # request: a backend settles them itself, and x264 forces adaptive quantisation\n"
        "  # on whatever the caller said, because its quant_offsets do nothing without it.\n"
        "  # \"unknown\" means this backend cannot report the field, which is not \"off\".\n"
        "  b_frames: %s\n"
        "  lookahead: %s\n"
        "  spatial_aq: %s\n"
        "  temporal_aq: %s\n"
        "\n"
        "input:\n"
        "  width: %d\n"
        "  height: %d\n"
        "  fps: %d\n"
        "  resolution_class: %s\n"
        "  bpppf: %.6f\n",
        opt.hardware_class.c_str(), opt.dataset.c_str(), opt.point,
        opt.backend.c_str(), stats.backend_name.c_str(), opt.codec.c_str(),
        library.name, library.version, sve::preset_name(cfg.settings.preset),
        rate_control, static_cast<unsigned long long>(target_bitrate), gop,
        sve::quality_policy_name(cfg.settings.policy),
        sve::fallback_policy_name(cfg.settings.fallback),
        number_or_unknown(effective.b_frames).c_str(),
        number_or_unknown(effective.lookahead_frames).c_str(),
        flag_or_unknown(effective.spatial_aq).c_str(),
        flag_or_unknown(effective.temporal_aq).c_str(),
        opt.width, opt.height, opt.fps, points->resolution, bpppf);
    if (written < 0 || written >= static_cast<int>(sizeof(body))) {
        std::fprintf(stderr, "manifest does not fit; widen the buffer\n");
        return 1;
    }

    std::printf("# TZ v4.2 §38 baseline manifest. Generated by sve-baseline -- do not edit:\n"
                "# regenerate and commit. Changing any field below changes profile_version,\n"
                "# which is what §38 asks for.\n"
                "profile_version: %016llx\n"
                "%s",
        static_cast<unsigned long long>(fingerprint_of(body)), body);
    return 0;
}
