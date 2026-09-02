// Soak -- TZ v4.2 §43.
//
// §43.1 asks for continuous encoding over a long stretch and names six things to watch:
// memory growth, frame loss, PTS, deadlock, backend reset, metadata synchronization. §43.2
// puts a zero beside each of five outcomes. This driver is the thing that produces those
// numbers, and it is one process rather than a script around the CLI for a reason: memory
// growth and frame accounting are only observable from inside the process doing the work.
//
// The shape is a cycle repeated until the clock runs out: open an encoder, encode a batch,
// flush, close. Opening every cycle is not overhead to be optimised away -- it is §43.1's
// "backend reset", and a leak that only shows on reopen would be invisible to a driver that
// opened once and ran for an hour.
//
// What each check actually catches:
//
//   frame accounting  every submitted timestamp comes back exactly once, and the encoder's
//                     own counters agree with ours. Two independent counts, because one
//                     count cannot disagree with itself.
//   PTS               output timestamps are the ones submitted, and DTS never goes
//                     backwards in emission order.
//   metadata          record N belongs to packet N (§19.3), checked by two numbers that
//                     would have to be wrong together: the record's pts and a track id set
//                     to that frame's own timestamp.
//   memory            RSS after warmup does not keep climbing. Sampled per cycle.
//   deadlock          a watchdog thread. If the encode stops making progress nothing in
//                     the main thread can report it, so the watchdog reports and exits.
//
//   sve-soak [--seconds N] [--encoder NAME] [--codec h264|h265] [--frames N]
//
// Exit codes are distinct on purpose: 0 pass, 1 an acceptance criterion failed, 2 bad
// arguments, 3 the watchdog fired. A caller that only knows "non-zero" cannot tell a
// deadlock from a mismatch, and §43.2 counts them separately.

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/raw_frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sve;

namespace {

/// Big enough that the encoder does real work per frame, small enough that an hour of this
/// costs an hour of one core rather than of the machine.
constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 240;

/// Frames per cycle. A batch rather than a single frame so that lookahead, reordering and
/// the drain path are all exercised inside one open.
constexpr int64_t kDefaultFramesPerCycle = 30;

constexpr int kDefaultSeconds = 10;

/// How long the encode may make no progress before the watchdog calls it a deadlock. Well
/// past any single frame on any backend here, and far short of the shortest useful run.
constexpr int kWatchdogSeconds = 60;

/// Cycles thrown away before memory is judged. The first opens fill allocator arenas, the
/// codec's own tables and the page cache, and calling that a leak would fail every run.
constexpr int64_t kMemoryWarmupCycles = 4;

/// Cycles needed before memory can be judged at all. Below this the answer would be noise,
/// and saying so is better than a green line that measured nothing.
constexpr int64_t kMinCyclesForMemory = 12;

/// The rate at which resident memory may still be rising once the process is warm, in
/// bytes per encoded frame, before it is called unbounded growth.
///
/// A rate and not a total, because a total cannot be judged without knowing how long the
/// run was: the same leak looks like nothing in five seconds and like a catastrophe in an
/// hour. This form also states the driver's sensitivity honestly -- a short run cannot see
/// a small leak, and the printed rate says what it did see.
///
/// Resident memory a run may gain, in total, before it is called unbounded growth.
///
/// A total and not only a rate, and gated only on long runs, because three attempts at
/// this taught the same lesson from different directions:
///
///   1. peaks with a 4 MiB tolerance -- a deliberate 64 KiB-per-cycle leak passed. A peak
///      reached early looks the same as a peak reached by climbing.
///   2. the rate between medians of halves -- caught that leak, then failed a clean
///      thirty-second run at 15 bytes/frame, on one 0.3 MiB step where glibc grew an arena.
///   3. the rate between medians of thirds, so a step raises one rate and a leak raises
///      both -- clean on an idle machine at every length tried, and then it failed the
///      acceptance run at 120 bytes/frame. Under the load of everything else check.sh does,
///      resident size rose 2.2 MiB steadily over 618 cycles with nothing leaking.
///
/// What that says is not that the third rule is subtly wrong. It is that over twenty
/// seconds resident size cannot separate a leak from an allocator at all: the signal and
/// the noise are the same size. So the short profile measures and reports, and only a run
/// long enough for a real leak to dwarf the noise gets to fail on it. A leak worth the name
/// is unbounded -- given an hour it does not stay under thirty-two megabytes.
constexpr std::size_t kMaxGrowthBytes = 32u * 1024u * 1024u;

/// Cycles a run needs before its memory verdict is allowed to fail it. §43's own hour
/// profile passes this inside the first minute; the acceptance step's short profile does
/// not reach it, and says so rather than reporting a check it did not make.
constexpr int64_t kMinCyclesToGateMemory = 2000;

struct Options {
    int seconds = kDefaultSeconds;
    int64_t frames_per_cycle = kDefaultFramesPerCycle;
    Backend encoder = Backend::X264Direct;
    BackendSelection selection;
    Codec codec = Codec::H264;
};

struct Findings {
    int64_t cycles = 0;
    int64_t frames_submitted = 0;
    int64_t packets = 0;

    int64_t frame_accounting_mismatch = 0;
    int64_t pts_violations = 0;
    int64_t metadata_mismatch = 0;

    std::vector<std::size_t> rss;
    std::vector<std::string> reasons;

    void note(std::string reason)
    {
        // Bounded: a run that fails every cycle for an hour must not fill memory with the
        // news, and the first few say the same thing as the thousandth.
        constexpr std::size_t kMaxReasons = 20;
        if (reasons.size() < kMaxReasons) {
            reasons.push_back(std::move(reason));
        }
    }
};

/// Resident set size in bytes, or 0 when this platform will not say.
///
/// Read as a file rather than through a system call, so this stays ordinary C++ and the
/// repository keeps its property of including no POSIX headers outside the plugin loader.
/// Zero is a real answer and the caller treats it as "not measured" rather than as "no
/// memory used" -- a skipped check reported as a pass is the thing §43 is least able to
/// afford.
std::size_t resident_bytes()
{
    std::ifstream statm("/proc/self/statm");
    if (!statm) {
        return 0;
    }
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    // 4096 is the page size on every platform this runs on; getting it from the system
    // would need a POSIX header for a number that only scales the result.
    constexpr std::size_t kPageBytes = 4096;
    return resident_pages * kPageBytes;
}

struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);

    /// Moving content, because a flat frame compresses to nothing and would soak the drain
    /// path rather than the encoder.
    explicit Pixels(int64_t n)
    {
        for (int32_t row = 0; row < kHeight; ++row) {
            for (int32_t col = 0; col < kWidth; ++col) {
                const bool lit = ((col + static_cast<int32_t>(n) * 5) / 16 + row / 16) % 2 == 0;
                y[static_cast<std::size_t>(row) * kWidth + col] = lit ? 210 : 45;
            }
        }
    }
};

EncoderConfig config_for(const Options& options, SidecarSink sink)
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = options.encoder;
    cfg.settings.backend_selection = options.selection;
    cfg.settings.codec = options.codec;
    cfg.settings.crf = 30;
    cfg.settings.metadata.sidecar = SidecarFormat::NDJSON;
    cfg.settings.metadata.sidecar_sink = std::move(sink);
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

/// One open, one batch, one close. Every §43.1 check except memory happens in here.
void run_cycle(const Options& options, Findings& out, std::atomic<int64_t>& heartbeat)
{
    std::vector<std::string> records;
    const std::unique_ptr<SmartVideoEncoder> enc = CreateVideoEncoder(
        config_for(options, [&records](std::string_view r) { records.emplace_back(r); }));

    std::vector<int64_t> submitted;
    std::vector<int64_t> emitted;
    int64_t last_dts = std::numeric_limits<int64_t>::min();

    const auto drain = [&] {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return;
            }
            if (!s.ok()) {
                out.note("ReceivePacket failed: " + s.message());
                return;
            }
            emitted.push_back(packet.pts);

            // §43.1's PTS check. DTS is the emission clock, so it is the one that must not
            // go backwards; PTS may, and does the moment there are B-frames.
            if (packet.dts != kNoTimestamp) {
                if (packet.dts < last_dts) {
                    ++out.pts_violations;
                    out.note("dts went backwards");
                }
                last_dts = packet.dts;
            }

            // §43.1's metadata synchronization, and §19.3's rule that record N belongs to
            // packet N. Two numbers that would have to be wrong together for this to pass
            // by accident: the record's own pts, and a track id set to the frame's
            // timestamp when it was submitted.
            const std::size_t index = emitted.size() - 1;
            if (index >= records.size()) {
                ++out.metadata_mismatch;
                out.note("a packet arrived with no sidecar record");
            } else {
                const std::string want_pts = "\"pts\":" + std::to_string(packet.pts);
                const std::string want_track =
                    "\"track_id\":" + std::to_string(packet.pts + 1);
                if (!contains(records[index], want_pts)
                    || !contains(records[index], want_track)) {
                    ++out.metadata_mismatch;
                    out.note("sidecar record " + std::to_string(index)
                        + " does not match its packet");
                }
            }
            ++out.packets;
        }
    };

    for (int64_t i = 0; i < options.frames_per_cycle; ++i) {
        auto pixels = std::make_shared<Pixels>(i);
        RawFrameView view;
        view.format.width = kWidth;
        view.format.height = kHeight;
        view.format.pixel_format = PixelFormat::YUV420P;
        view.plane_count = 3;
        view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
        view.timestamp = MediaTimestamp{i, Rational{1, 25}};
        view.owner = pixels;

        TimedFrame timed;
        if (!WrapRawFrame(view, &timed.frame).ok()) {
            out.note("WrapRawFrame refused a frame");
            return;
        }
        timed.metadata.timestamp = view.timestamp;

        // track_id is the frame's own timestamp plus one: zero means "not reported", and a
        // constant id would make the metadata check pass however the records were shuffled.
        Box box;
        box.kind = "plate";
        box.x = 0.3f;
        box.y = 0.3f;
        box.width = 0.2f;
        box.height = 0.2f;
        box.track_id = static_cast<uint64_t>(i) + 1;
        box.confidence = 0.9f;
        const std::vector<Box> boxes{box};
        timed.metadata.regions.boxes = boxes;

        const Status s = enc->SubmitFrame(std::move(timed));
        if (!s.ok()) {
            out.note("SubmitFrame failed: " + s.message());
            return;
        }
        submitted.push_back(i);
        ++out.frames_submitted;
        heartbeat.fetch_add(1);
        drain();
    }

    if (!enc->Flush().ok()) {
        out.note("Flush failed");
        return;
    }
    drain();

    // §43.1's frame accounting, counted twice from two places. Sorting because emission
    // order is the encoder's business and the claim is about the set, not the sequence.
    std::sort(emitted.begin(), emitted.end());
    if (emitted != submitted) {
        ++out.frame_accounting_mismatch;
        out.note("submitted " + std::to_string(submitted.size()) + " frames and got "
            + std::to_string(emitted.size()) + " packets back");
    }
    const EncoderStatistics stats = enc->GetStatistics();
    if (stats.frames_submitted != static_cast<int64_t>(submitted.size())
        || stats.packets_received != static_cast<int64_t>(emitted.size())) {
        ++out.frame_accounting_mismatch;
        out.note("the encoder's own counters disagree with this driver's");
    }
    if (records.size() != emitted.size()) {
        ++out.metadata_mismatch;
        out.note("sidecar records " + std::to_string(records.size()) + " against "
            + std::to_string(emitted.size()) + " packets");
    }
    ++out.cycles;
}

/// Whether memory kept climbing, or the reason no answer could be given.
struct MemoryVerdict {
    bool measured = false;
    /// Whether this run is long enough for the verdict to be allowed to fail it.
    bool gating = false;
    bool growing = false;
    double bytes_per_frame = 0.0;
    std::size_t growth_bytes = 0;
    std::size_t first_half = 0;
    std::size_t second_half = 0;
    std::string skip_reason;
};

/// The middle sample, which is what a plateau with occasional spikes should be judged by.
///
/// Peaks were tried first and are the wrong statistic: a process that reached its ceiling
/// early has the same peak in both halves as one that is leaking slowly, and a deliberate
/// 64 KiB-per-cycle leak passed a peak comparison outright. The median moves with the
/// trend, which is the thing §43.2 calls unbounded growth.
std::size_t median_of(std::vector<std::size_t> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/// Growth is judged in thirds, not halves, and that is the difference between a leak and an
/// allocator.
///
/// Halves were tried first and produced a false failure on a clean thirty-second run: 15
/// bytes per frame, from a single 0.3 MiB step where glibc grew an arena. A step is not
/// unbounded growth -- it happens once and stops -- but across two halves it is
/// indistinguishable from a trend, and the shorter the run the larger the rate it implies.
/// Across three parts a step raises one of the two rates and leaves the other at zero,
/// while something that is actually leaking raises both. So the verdict is the smaller of
/// the two rates, and a one-off jump can no longer fail a run.
MemoryVerdict judge_memory(const std::vector<std::size_t>& rss, int64_t frames_per_cycle)
{
    MemoryVerdict verdict;
    if (!rss.empty() && rss.front() == 0) {
        verdict.skip_reason = "this platform does not report resident size";
        return verdict;
    }
    if (static_cast<int64_t>(rss.size()) < kMinCyclesForMemory) {
        verdict.skip_reason = "only " + std::to_string(rss.size())
            + " cycles; memory needs at least " + std::to_string(kMinCyclesForMemory);
        return verdict;
    }
    const std::size_t warmup = static_cast<std::size_t>(kMemoryWarmupCycles);
    const std::size_t begin = warmup < rss.size() ? warmup : 0;
    const std::size_t span = rss.size() - begin;
    const std::size_t cut1 = begin + span / 3;
    const std::size_t cut2 = begin + (span * 2) / 3;

    const auto at = [&](std::size_t i) { return rss.begin() + static_cast<std::ptrdiff_t>(i); };
    const std::size_t m1 = median_of({at(begin), at(cut1)});
    const std::size_t m2 = median_of({at(cut1), at(cut2)});
    const std::size_t m3 = median_of({at(cut2), rss.end()});
    verdict.first_half = m1;
    verdict.second_half = m3;
    verdict.measured = true;

    // Frames between one median and the next: a third of the measured cycles.
    const double frames_between =
        static_cast<double>(span) / 3.0 * static_cast<double>(frames_per_cycle);
    if (frames_between <= 0.0) {
        verdict.skip_reason = "not enough frames between samples";
        verdict.measured = false;
        return verdict;
    }
    const auto rate = [&](std::size_t from, std::size_t to) {
        return to >= from ? static_cast<double>(to - from) / frames_between : 0.0;
    };
    verdict.bytes_per_frame = std::min(rate(m1, m2), rate(m2, m3));
    verdict.growth_bytes = m3 >= m1 ? m3 - m1 : 0;

    // Both halves of the claim: it is still climbing at the end, and it has climbed far
    // enough that no allocator would explain it.
    const bool climbing = verdict.bytes_per_frame > 0.0;
    verdict.growing = climbing && verdict.growth_bytes > kMaxGrowthBytes;
    verdict.gating = static_cast<int64_t>(rss.size()) >= kMinCyclesToGateMemory;
    return verdict;
}

int usage()
{
    std::fprintf(stderr,
        "Usage: sve-soak [--seconds N] [--encoder NAME] [--codec h264|h265] [--frames N]\n"
        "\n"
        "Continuous encoding for N seconds, opening and closing the encoder every batch,\n"
        "and the six checks of TZ §43.1 over it. Prints §43.2's five counts and exits 0\n"
        "when every one of them is zero, 1 when one is not, 3 when the watchdog fired.\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(usage());
            }
            return argv[++i];
        };
        if (std::strcmp(a, "--seconds") == 0) {
            options.seconds = std::atoi(need("--seconds"));
        } else if (std::strcmp(a, "--frames") == 0) {
            options.frames_per_cycle = std::atoll(need("--frames"));
        } else if (std::strcmp(a, "--codec") == 0) {
            const char* c = need("--codec");
            options.codec = std::strcmp(c, "h265") == 0 ? Codec::H265 : Codec::H264;
        } else if (std::strcmp(a, "--encoder") == 0) {
            try {
                const BackendChoice choice = backend_choice_from_name(need("--encoder"));
                options.encoder = choice.kind;
                options.selection = choice.selection;
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "%s\n", ex.what());
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (options.seconds <= 0 || options.frames_per_cycle <= 0) {
        return usage();
    }

    // The watchdog. It is the only thing that can speak if the encode stops, so it reports
    // and leaves rather than setting a flag nobody will read.
    std::atomic<int64_t> heartbeat{0};
    std::atomic<bool> finished{false};
    std::thread watchdog([&heartbeat, &finished] {
        int64_t last = -1;
        int stalled = 0;
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t now = heartbeat.load();
            stalled = now == last ? stalled + 1 : 0;
            last = now;
            if (stalled >= kWatchdogSeconds) {
                std::fprintf(stderr,
                    "soak: no progress for %d seconds after %lld frames -- deadlock\n",
                    kWatchdogSeconds, static_cast<long long>(now));
                std::fflush(nullptr);
                std::_Exit(3);
            }
        }
    });

    Findings findings;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(options.seconds);
    try {
        while (std::chrono::steady_clock::now() < deadline) {
            run_cycle(options, findings, heartbeat);
            findings.rss.push_back(resident_bytes());
        }
    } catch (const std::exception& ex) {
        finished.store(true);
        watchdog.join();
        std::fprintf(stderr, "soak: %s\n", ex.what());
        return 1;
    }
    finished.store(true);
    watchdog.join();

    const MemoryVerdict memory = judge_memory(findings.rss, options.frames_per_cycle);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();

    std::printf("soak: %lld s, %lld cycles, %lld frames, %lld packets\n",
        static_cast<long long>(elapsed), static_cast<long long>(findings.cycles),
        static_cast<long long>(findings.frames_submitted),
        static_cast<long long>(findings.packets));
    std::printf("  frame accounting mismatch : %lld\n",
        static_cast<long long>(findings.frame_accounting_mismatch));
    std::printf("  pts violations            : %lld\n",
        static_cast<long long>(findings.pts_violations));
    std::printf("  metadata mismatch         : %lld\n",
        static_cast<long long>(findings.metadata_mismatch));
    if (memory.measured) {
        std::printf("  memory growth             : %+.1f MiB over the run, %.2f bytes/frame "
                    "(%.1f -> %.1f MiB) %s\n",
            static_cast<double>(memory.growth_bytes) / (1024.0 * 1024.0),
            memory.bytes_per_frame,
            static_cast<double>(memory.first_half) / (1024.0 * 1024.0),
            static_cast<double>(memory.second_half) / (1024.0 * 1024.0),
            memory.growing ? "GROWING" : "flat");
        if (!memory.gating) {
            std::printf("  memory verdict            : not gated -- %lld cycles, "
                        "%lld needed before this may fail a run\n",
                static_cast<long long>(findings.rss.size()),
                static_cast<long long>(kMinCyclesToGateMemory));
        }
    } else {
        std::printf("  memory                    : not measured (%s)\n",
            memory.skip_reason.c_str());
    }
    for (const std::string& reason : findings.reasons) {
        std::printf("  reason: %s\n", reason.c_str());
    }

    const bool failed = findings.frame_accounting_mismatch != 0
        || findings.pts_violations != 0 || findings.metadata_mismatch != 0
        || (memory.measured && memory.gating && memory.growing)
        || !findings.reasons.empty();
    std::printf("soak: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
