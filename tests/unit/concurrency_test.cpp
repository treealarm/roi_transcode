// What holds when this library is used from several threads at once.
//
// There is no worker thread here and no mutex: §Р6 keeps the facade thin and §Р7 makes the
// backends stateless immortal singletons, so concurrency in this library is entirely the
// caller's. That is a claim, and until 2026-09-01 it was only a claim -- nothing anywhere
// ran two encoders at the same time. This file is what turns it into something checked.
//
// Three properties, and they are the ones a caller actually relies on:
//
//   1. Encoders on different threads do not see each other. Same input, same settings, same
//      bytes out -- whether one thread does it or eight do it at once.
//   2. The shared first-use state is safe to touch simultaneously. The registry, the
//      backend singletons and the oneVPL capability memo are all function-local statics
//      filled on first call, and every thread here is released at the same instant so that
//      the first call really is contended rather than merely repeated.
//   3. Capabilities answer the same thing to everyone.
//
// What this file deliberately does NOT test: one encoder driven from two threads. That is
// not supported -- SmartVideoEncoder holds plain, unguarded state -- and a test that did it
// would be testing undefined behaviour, which passes and proves nothing. It is written down
// in the header instead.
//
// A green run is weaker evidence than it looks: a data race can stay invisible for a
// thousand runs. Under ThreadSanitizer it is not -- integration/scripts/tsan-check.sh
// builds exactly this test with -fsanitize=thread and runs it, which is where the teeth are.

#include "sve/backend_registry.h"
#include "sve/encoder.h"
#include "sve/encoder_backend.h"
#include "sve/raw_frame.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sve;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 96;
constexpr int64_t kFrames = 8;

/// Threads enough to contend, few enough that eight encoders on one machine stay quick.
int worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned want = hw == 0 ? 4u : hw;
    return static_cast<int>(want < 2u ? 2u : (want > 8u ? 8u : want));
}

/// Everyone waits, then everyone goes. Without this the threads start in sequence and the
/// first-use paths are already warm by the time the second one arrives -- which is exactly
/// the case that cannot race, and exactly the one worth not testing.
class StartingGate {
public:
    explicit StartingGate(int expected) : m_expected(expected) {}

    void arrive_and_wait()
    {
        m_arrived.fetch_add(1);
        while (m_arrived.load() < m_expected) {
            std::this_thread::yield();
        }
    }

private:
    const int m_expected;
    std::atomic<int> m_arrived{0};
};

struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);

    /// A moving edge rather than flat grey: flat frames compress to almost nothing and
    /// would make "same bytes out" true for uninteresting reasons.
    explicit Pixels(int64_t n)
    {
        for (int32_t row = 0; row < kHeight; ++row) {
            for (int32_t col = 0; col < kWidth; ++col) {
                const bool lit = ((col + static_cast<int32_t>(n) * 3) / 8 + row / 8) % 2 == 0;
                y[static_cast<std::size_t>(row) * kWidth + col] = lit ? 220 : 40;
            }
        }
    }
};

EncoderConfig config_for()
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = Backend::X264Direct;
    cfg.settings.codec = Codec::H264;
    cfg.settings.crf = 32;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

TimedFrame frame_at(int64_t pts)
{
    auto pixels = std::make_shared<Pixels>(pts);
    RawFrameView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_format = PixelFormat::YUV420P;
    view.plane_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.timestamp = MediaTimestamp{pts, Rational{1, 25}};
    view.owner = pixels;

    TimedFrame timed;
    CHECK(WrapRawFrame(view, &timed.frame).ok());
    timed.metadata.timestamp = view.timestamp;
    return timed;
}

/// One whole encode, reduced to something two runs can be compared by.
///
/// Not a hash: when this fails, the packet sizes say whether the streams diverged at the
/// first frame or the last, and a hash would say only "different".
struct Digest {
    std::vector<std::size_t> packet_sizes;
    std::size_t total_bytes = 0;

    bool operator==(const Digest& other) const
    {
        return packet_sizes == other.packet_sizes && total_bytes == other.total_bytes;
    }
};

Digest encode_once()
{
    Digest digest;
    const std::unique_ptr<SmartVideoEncoder> enc = CreateVideoEncoder(config_for());

    const auto drain = [&] {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return;
            }
            CHECK(s.ok());
            digest.packet_sizes.push_back(packet.payload.size());
            digest.total_bytes += packet.payload.size();
        }
    };

    for (int64_t i = 0; i < kFrames; ++i) {
        TimedFrame timed = frame_at(i);
        CHECK(enc->SubmitFrame(timed).ok());
        drain();
    }
    CHECK(enc->Flush().ok());
    drain();
    return digest;
}

// --- the tests --------------------------------------------------------------

/// Property 1, and the premise it rests on.
///
/// The premise is checked first and separately: if two serial runs already disagree, this
/// encoder is not deterministic and the parallel comparison would be reporting that rather
/// than a threading fault. Saying which of the two broke is the whole point.
void encoders_on_different_threads_produce_what_one_thread_produces()
{
    const Digest reference = encode_once();
    CHECK(!reference.packet_sizes.empty());
    CHECK(reference.total_bytes > 0);
    CHECK(encode_once() == reference);   // the premise: serial runs agree with each other

    const int workers = worker_count();
    StartingGate gate(workers);
    std::vector<Digest> results(static_cast<std::size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            gate.arrive_and_wait();
            results[static_cast<std::size_t>(i)] = encode_once();
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (const Digest& got : results) {
        CHECK(got == reference);
    }
}

/// Property 2. Every thread's first call lands on the same uninitialised statics: the
/// registry filled by RegisterDeploymentBackends, the backend singletons behind it, and --
/// on a machine with Intel silicon -- the oneVPL capability memo, which opens a dispatcher
/// and creates a session to answer.
void the_shared_registry_survives_being_reached_first_by_everyone()
{
    const int workers = worker_count();
    StartingGate gate(workers);
    std::vector<std::vector<Backend>> seen(static_cast<std::size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            gate.arrive_and_wait();
            seen[static_cast<std::size_t>(i)] = default_backend_registry().kinds();
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(!seen.front().empty());
    for (const std::vector<Backend>& kinds : seen) {
        // Same set and same order: registration order is what §23 reports and what
        // allow_unlisted walks, so a registry filled twice would be visible here.
        CHECK(kinds == seen.front());
    }
}

/// Property 3. GetCapabilities is const and is asked concurrently by selection; on the
/// native oneVPL backend it is also where a memoised device probe lives.
void capabilities_answer_the_same_thing_to_every_thread()
{
    BackendRegistry& registry = default_backend_registry();
    const std::vector<Backend> kinds = registry.kinds();
    CHECK(!kinds.empty());

    const auto snapshot = [&] {
        std::vector<std::string> out;
        for (const Backend kind : kinds) {
            const IEncoderBackend* backend = registry.Find(kind);
            CHECK(backend != nullptr);
            for (const Codec codec : {Codec::H264, Codec::H265}) {
                const EncoderCapabilities c = backend->GetCapabilities(codec);
                out.push_back(std::to_string(c.h264) + std::to_string(c.h265)
                    + std::to_string(c.roi_rectangles) + std::to_string(c.qp_map)
                    + std::to_string(c.qp_map_block_width) + std::to_string(c.max_roi_regions)
                    + std::to_string(c.native_bitrate) + std::to_string(c.emphasis_map));
            }
        }
        return out;
    };

    // The threads go first, so that on a machine with Intel silicon they land on the
    // oneVPL device memo cold. Taking the reference serially before them would fill it,
    // and the contended case -- the one worth testing -- would never happen.
    const int workers = worker_count();
    StartingGate gate(workers);
    std::vector<std::vector<std::string>> results(static_cast<std::size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            gate.arrive_and_wait();
            results[static_cast<std::size_t>(i)] = snapshot();
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    const std::vector<std::string>& reference = results.front();
    CHECK(!reference.empty());
    for (const std::vector<std::string>& got : results) {
        CHECK(got == reference);
    }
    // And the same answer once everything is warm, which is what a caller asking later gets.
    CHECK(snapshot() == reference);
}

} // namespace

int main()
{
    // Ordered so that the contended first-use paths run before anything has warmed them:
    // the registry test would prove much less after an encode had already filled it.
    the_shared_registry_survives_being_reached_first_by_everyone();
    capabilities_answer_the_same_thing_to_every_thread();
    encoders_on_different_threads_produce_what_one_thread_produces();
    std::printf("concurrency_test ok\n");
    return 0;
}
