// The C++ reference for §58.10.8's wrapper-overhead budget.
//
// §58.10 accepts the Python wrapper only if, for a CPU frame submit that needs no copy, the
// P95 overhead stays within 5% of the equivalent C++ call on the same profile. That is a
// number nobody had: the wrapper was known to work and not known to be cheap.
//
// **What is timed.** The per-frame path is the two calls that differ between the languages,
// and it is timed in two regions rather than one:
//
//     WrapRawFrame          <->  frame_from_planes + FrameMetadata     "wrap"
//     SubmitFrame           <->  submit_frame                          "submit"
//
// The split is what §58.10.8 means by measuring the overhead separately from encode latency.
// `wrap` contains no encoder at all, so it is the wrapper's own cost with nothing else in
// it; `submit` reaches into x264 and is dominated by the encode, which is identical work on
// both sides. `total` is what an application pays per frame, and its ratio is the criterion.
//
// **The profile is 720p30, the smallest operating point of §39, and that matters.** The
// wrapper's cost per frame is a constant of a few microseconds -- it does not copy pixels
// (§58.4) -- so the ratio it forms is decided entirely by what it is divided by. Measured on
// a 160x120 toy the same wrapper looks like a 9% tax, and on real frames like a fraction of
// a percent. Neither number is wrong; only one of them is about a profile anybody encodes.
//
// Prints P50 and P95 of all three regions in nanoseconds. integration/scripts/
// python-overhead.py prints the same lines through the wrapper, and check.sh compares them.
//
// A timing test inside an acceptance run is a flaky test unless it is built not to be. Four
// things here are: 300 samples, so P95 is the 15th slowest rather than a coin toss; a warm-up
// pass that is discarded, because the first submits pay for allocation inside x264 that later
// ones do not; a GOP long enough that no keyframe lands in the measured window, since at
// GOP 10 the top 5% of samples were simply the keyframes; and check.sh taking the best of
// several repetitions, because noise on a loaded machine only ever makes a run slower.

#include "sve/encoder.h"
#include "sve/raw_frame.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace sve;

namespace {

/// 720p30, the smallest operating point of §39 -- see the note at the top about why the
/// resolution decides the answer.
constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;
constexpr int32_t kFps = 30;
constexpr int32_t kCrf = 30;
/// Longer than the whole run on purpose: only frame 0 is a keyframe, and frame 0 is inside
/// the discarded warm-up. At GOP 10 every tenth sample was an I-frame, which put P95 inside
/// the keyframe population and made this measure keyframe cost instead of wrapper cost.
constexpr int32_t kGop = 100000;

/// Enough that P95 is a percentile rather than a coin toss: at 300 samples it is the 15th
/// slowest, so a single stall cannot define it.
constexpr int kSamples = 300;
/// Discarded. The first submits pay for allocation inside x264 that later ones do not, and
/// including them would measure the warm-up on whichever side ran first.
constexpr int kWarmup = 30;

struct Planes {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);

    explicit Planes(int64_t n)
    {
        for (int32_t row = 0; row < kHeight; ++row) {
            for (int32_t col = 0; col < kWidth; ++col) {
                const bool lit = ((col + static_cast<int32_t>(n) * 3) / 8 + row / 8) % 2 == 0;
                y[static_cast<std::size_t>(row) * kWidth + col] = lit ? 220 : 40;
            }
        }
    }
};

double percentile(std::vector<double>& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted_samples.size() - 1));
    return sorted_samples[index];
}

} // namespace

int main()
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{kFps, 1};
    cfg.input.time_base = Rational{1, kFps};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = Backend::X264Direct;
    cfg.settings.backend_selection.preferred = {Backend::X264Direct};
    cfg.settings.codec = Codec::H264;
    cfg.settings.crf = kCrf;
    cfg.settings.gop = kGop;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};

    std::unique_ptr<SmartVideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "submit_overhead: cannot open x264direct: %s\n", e.what());
        return 2;
    }

    // Built before the clock starts, for the reason in the header comment.
    const int total = kSamples + kWarmup;
    std::vector<std::shared_ptr<Planes>> frames;
    frames.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        frames.push_back(std::make_shared<Planes>(i));
    }

    const auto drain = [&] {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return;
            }
            if (!s.ok()) {
                return;
            }
        }
    };

    std::vector<double> wrap_ns;
    std::vector<double> submit_ns;
    std::vector<double> total_ns;
    wrap_ns.reserve(static_cast<std::size_t>(kSamples));
    submit_ns.reserve(static_cast<std::size_t>(kSamples));
    total_ns.reserve(static_cast<std::size_t>(kSamples));

    for (int i = 0; i < total; ++i) {
        const std::shared_ptr<Planes>& pixels = frames[static_cast<std::size_t>(i)];
        const auto started = std::chrono::steady_clock::now();

        RawFrameView view;
        view.format = cfg.input;
        view.plane_count = 3;
        view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
        view.timestamp = MediaTimestamp{i, Rational{1, kFps}};
        view.owner = pixels;

        TimedFrame timed;
        if (const Status s = WrapRawFrame(view, &timed.frame); !s.ok()) {
            std::fprintf(stderr, "submit_overhead: wrap: %s\n", s.message().c_str());
            return 2;
        }
        timed.metadata.timestamp = view.timestamp;

        const auto wrapped = std::chrono::steady_clock::now();
        if (const Status s = enc->SubmitFrame(timed); !s.ok()) {
            std::fprintf(stderr, "submit_overhead: submit: %s\n", s.message().c_str());
            return 2;
        }
        const auto finished = std::chrono::steady_clock::now();

        if (i >= kWarmup) {
            const auto ns = [](auto from, auto to) {
                return std::chrono::duration<double, std::nano>(to - from).count();
            };
            wrap_ns.push_back(ns(started, wrapped));
            submit_ns.push_back(ns(wrapped, finished));
            total_ns.push_back(ns(started, finished));
        }
        // Outside the measurement: receiving is not the call being compared, and leaving
        // the queue to fill would time back pressure instead of a submit.
        drain();
    }

    (void)enc->Flush();
    drain();

    std::printf("samples %zu\n", total_ns.size());
    const auto report = [](const char* name, std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        std::printf("%s_p50_ns %.0f\n", name, percentile(v, 0.50));
        std::printf("%s_p95_ns %.0f\n", name, percentile(v, 0.95));
    };
    report("wrap", wrap_ns);
    report("submit", submit_ns);
    report("total", total_ns);
    return 0;
}
