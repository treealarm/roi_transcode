// The callback API -- TZ v3.4 §8.8 and §8.9.
//
// What matters here is the shape of the conversation: a packet never belongs to the frame
// just handed over, the end of the stream is an event rather than a null, and a failure
// reaches the caller as an event too. The picture is the pull API's business and is tested
// there; this file is about who gets told what, and when.

#include "sve/frame.h"
#include "sve/push_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
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

struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

EncoderConfig config_for()
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{25, 1};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = Backend::X264;
    cfg.settings.codec = Codec::H264;
    cfg.settings.crf = 32;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

TimedFrame frame_at(const std::shared_ptr<Pixels>& pixels, int64_t pts)
{
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

std::unique_ptr<PushVideoEncoder> open_push(const EncoderConfig& cfg)
{
    return CreatePushVideoEncoder(cfg);
}

/// What the callback saw, in order.
struct Recorder {
    std::vector<EncoderEventType> types;
    int64_t packets = 0;
    int64_t bytes = 0;
    Status last_error;

    EncoderCallback sink()
    {
        return [this](EncoderEvent event) {
            types.push_back(event.type);
            if (event.type == EncoderEventType::Packet) {
                CHECK(event.packet != nullptr);
                CHECK(!event.packet->payload.empty());
                ++packets;
                bytes += static_cast<int64_t>(event.packet->payload.size());
            } else {
                // §8.9: everything that is not a packet carries no packet at all, rather
                // than a null one the caller has to remember to check.
                CHECK(event.packet == nullptr);
            }
            if (event.type == EncoderEventType::Error) {
                last_error = event.status;
            }
        };
    }
};

void packets_arrive_through_the_callback_and_the_stream_ends_with_an_event()
{
    auto pixels = std::make_shared<Pixels>();
    Recorder seen;
    auto push = open_push(config_for());
    CHECK(push->Subscribe(seen.sink()).ok());

    for (int64_t i = 0; i < 8; ++i) {
        CHECK(push->Process(frame_at(pixels, i)).ok());
    }
    CHECK(push->Flush().ok());

    CHECK(seen.packets == 8);
    CHECK(seen.bytes > 0);
    CHECK(!seen.types.empty());
    CHECK(seen.types.back() == EncoderEventType::EndOfStream);

    // Exactly one end, however many times it is asked for.
    CHECK(push->Flush().ok());
    int64_t ends = 0;
    for (const EncoderEventType t : seen.types) {
        if (t == EncoderEventType::EndOfStream) {
            ++ends;
        }
    }
    CHECK(ends == 1);

    const EncoderStatistics stats = push->GetStatistics();
    CHECK(stats.frames_submitted == 8);
    CHECK(stats.packets_received == seen.packets);
}

void the_input_format_is_fixed_once_frames_have_started()
{
    auto pixels = std::make_shared<Pixels>();
    Recorder seen;

    EncoderConfig cfg = config_for();
    cfg.input = InputVideoFormat{};   // deliberately empty: it arrives separately
    auto push = open_push(cfg);
    CHECK(push->Subscribe(seen.sink()).ok());

    InputVideoFormat format;
    format.width = kWidth;
    format.height = kHeight;
    format.frame_rate = Rational{25, 1};
    format.pixel_format = PixelFormat::YUV420P;
    CHECK(push->SetInputFormat(format).ok());
    CHECK(push->State() == EncoderState::Created);

    CHECK(push->Process(frame_at(pixels, 0)).ok());
    CHECK(push->State() == EncoderState::Running);

    // §8.3: changing it now would be a different encode, not a reconfiguration of this one.
    CHECK(push->SetInputFormat(format).code() == StatusCode::InvalidArgument);
}

void a_format_that_cannot_exist_is_refused_before_anything_opens()
{
    auto push = open_push(config_for());
    InputVideoFormat broken;
    broken.width = 0;
    CHECK(push->SetInputFormat(broken).code() == StatusCode::InvalidArgument);
}

void a_backend_that_will_not_open_arrives_as_an_error_event()
{
    Recorder seen;
    EncoderConfig cfg = config_for();
    cfg.settings.encoder = Backend::Auto;
    // Nothing else in the order, so the chain is exhausted rather than falling to
    // software -- which since 4.1 is said by leaving software off the list.
    cfg.backend_selection.preferred = {Backend::NvidiaNvenc};

    auto pixels = std::make_shared<Pixels>();
    auto push = open_push(cfg);
    CHECK(push->Subscribe(seen.sink()).ok());

    // Create said nothing: the backend is opened on first use, so this is where it fails.
    CHECK(!push->Process(frame_at(pixels, 0)).ok());
    CHECK(!seen.types.empty());
    CHECK(seen.types.front() == EncoderEventType::Error);
    CHECK(!seen.last_error.ok());
}

void flushing_an_encoder_that_never_ran_still_ends_the_stream()
{
    Recorder seen;
    auto push = open_push(config_for());
    CHECK(push->Subscribe(seen.sink()).ok());
    CHECK(push->Flush().ok());
    CHECK(seen.types.size() == 1);
    CHECK(seen.types.front() == EncoderEventType::EndOfStream);
}

void no_subscriber_is_not_a_failure()
{
    // A caller who only wants the file on disk has nowhere to put events, and dropping them
    // is the right answer rather than an error nobody asked for.
    auto pixels = std::make_shared<Pixels>();
    auto push = open_push(config_for());
    CHECK(push->Process(frame_at(pixels, 0)).ok());
    CHECK(push->Flush().ok());
    CHECK(push->Stop().ok());
    CHECK(push->Stop().ok());
}

void a_missing_frame_is_an_argument_not_a_crash()
{
    auto push = open_push(config_for());
    CHECK(push->Process(TimedFrame{}).code() == StatusCode::InvalidArgument);
}

} // namespace

int main()
{
    packets_arrive_through_the_callback_and_the_stream_ends_with_an_event();
    the_input_format_is_fixed_once_frames_have_started();
    a_format_that_cannot_exist_is_refused_before_anything_opens();
    a_backend_that_will_not_open_arrives_as_an_error_event();
    flushing_an_encoder_that_never_ran_still_ends_the_stream();
    no_subscriber_is_not_a_failure();
    a_missing_frame_is_an_argument_not_a_crash();
    std::printf("ok\n");
    return 0;
}
