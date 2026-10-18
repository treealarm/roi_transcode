// The decoder's side: what a caller promises about its buffers, and what that costs when it
// is not asked.
//
// One property is worth a test of its own. An Annex-B access unit ends where the next one
// begins and nowhere else, so a parser in front of the decoder cannot emit a picture until
// the following buffer arrives -- it holds every picture back by exactly one access unit.
// On a stream pushed whole that is one frame of latency and invisible; on a caller that
// pushes only keyframes, which is what analytics does between samples, it is a whole group
// of pictures, and the frame it finally hands over is dated by the packet that dislodged
// it. Nothing about that fails loudly. So the test is: the first push produces the first
// picture.
//
// The stream comes from this library's own encoder rather than a fixture file. It is
// Annex-B with the parameter sets in band -- which is the case that would install a parser
// -- and it needs nothing checked in.

#include "ta_roienc/decoder.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"
#include "ta_roienc/access_unit.h"
#include "ta_roienc/picture_view.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ta_roienc;

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

struct Samples {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

/// kFrames access units of H.264, in encode order and one to a vector.
std::vector<std::vector<uint8_t>> annex_b_stream()
{
    EncodeOptions cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Ratio{25, 1};
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.encoder = Engine::X264;
    cfg.settings.codec = Coding::H264;
    cfg.settings.crf = 32;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](Verbosity, std::string_view) {};

    std::vector<std::vector<uint8_t>> units;
    auto encoder = CreateVideoEncoder(cfg);
    const auto drain = [&units, &encoder] {
        for (;;) {
            AccessUnit packet;
            const Verdict s = encoder->ReceiveAccessUnit(&packet);
            if (s.code() == VerdictCode::Again || s.code() == VerdictCode::EndOfStream) {
                return;
            }
            CHECK(s.ok());
            const std::span<const uint8_t> bytes = packet.payload.bytes();
            units.emplace_back(bytes.begin(), bytes.end());
        }
    };

    auto pixels = std::make_shared<Samples>();
    for (int64_t i = 0; i < kFrames; ++i) {
        // A moving edge, so the encoder has something to code and no two access units are
        // the same size. A flat picture would still exercise the timing, but a stream whose
        // packets are all alike hides an off-by-one between them.
        for (int32_t row = 0; row < kHeight; ++row) {
            const std::size_t start = static_cast<std::size_t>(row) * kWidth;
            for (int32_t col = 0; col < kWidth; ++col) {
                pixels->y[start + static_cast<std::size_t>(col)]
                    = static_cast<uint8_t>(col + i * 7 < kWidth / 2 ? 40 : 200);
            }
        }

        PictureView view;
        view.format.width = kWidth;
        view.format.height = kHeight;
        view.format.pixel_layout = PixelLayout::YUV420P;
        view.layer_count = 3;
        view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
        view.timestamp = Stamp{i, Ratio{1, 25}};
        view.owner = pixels;

        StampedPicture timed;
        CHECK(WrapPicture(view, &timed.frame).ok());
        timed.metadata.timestamp = view.timestamp;
        CHECK(encoder->SubmitPicture(timed).ok());
        drain();
    }
    CHECK(encoder->Flush().ok());
    drain();
    CHECK(static_cast<int64_t>(units.size()) == kFrames);
    return units;
}

void a_whole_access_unit_decodes_to_a_picture_without_the_next_one()
{
    const std::vector<std::vector<uint8_t>> units = annex_b_stream();

    VideoDecoder decoder;
    // No extradata: x264 leaves the parameter sets in the stream, which is the shape that
    // would otherwise be handed to a parser.
    CHECK(decoder.Opened(SourceCodec::H264, {}).ok());
    CHECK(decoder.decoder_name() == "h264");
    // Nothing was asked for, so nothing hardware may be reported.
    CHECK(decoder.hardware_name().empty());

    int64_t from_first = 0;
    CHECK(decoder
              .Push(units[0], Stamp{0, Ratio{1, 25}},
                  [&from_first](const Picture& frame, const DecodedPictureInfo& info) {
                      ++from_first;
                      CHECK(frame);
                      CHECK(frame.buffer().width() == kWidth);
                      CHECK(frame.buffer().height() == kHeight);
                      // The first picture of a stream is the one everything else refers to.
                      CHECK(info.key_frame);
                  })
              .ok());
    CHECK(from_first == 1);

    int64_t rest = 0;
    for (std::size_t i = 1; i < units.size(); ++i) {
        CHECK(decoder
                  .Push(units[i], Stamp{static_cast<int64_t>(i), Ratio{1, 25}},
                      [&rest](const Picture&, const DecodedPictureInfo&) { ++rest; })
                  .ok());
    }
    // Every picture, and none left inside: max_b_frames is 0 on this backend, so there is
    // no reordering to hold one back either.
    CHECK(1 + rest == kFrames);
    CHECK(decoder.report().frames_decoded == kFrames);
}

void a_cpu_frame_is_already_readable_and_downloads_to_itself()
{
    const std::vector<std::vector<uint8_t>> units = annex_b_stream();

    VideoDecoder decoder;
    CHECK(decoder.Opened(SourceCodec::H264, {}, DecodeDevice::Cpu()).ok());

    int64_t seen = 0;
    CHECK(decoder
              .Push(units[0], Stamp{0, Ratio{1, 25}},
                  [&](const Picture& frame, const DecodedPictureInfo&) {
                      ++seen;
                      // Download is what a caller writes whether or not a device is in play,
                      // and on the CPU it has to be free rather than refused.
                      Fallible<Picture> readable = decoder.Download(frame);
                      CHECK(readable.ok());
                      Picture& out = readable.value();
                      CHECK(out.buffer().format() == PixelLayout::YUV420P);
                      CHECK(!out.writable_data().plane(0).empty());
                  })
              .ok());
    CHECK(seen == 1);
    // Nothing crossed a bus, so nothing is counted as having done so.
    CHECK(decoder.report().frames_downloaded == 0);
    CHECK(decoder.report().download_failures == 0);
}

void the_cpu_device_is_a_value_rather_than_an_absence()
{
    const DecodeDevice cpu;
    CHECK(cpu.kind() == DecodeEngine::Cpu);
    CHECK(cpu.name() == decode_backend_name(DecodeEngine::Cpu));
    CHECK(cpu.skipped().empty());

    // Copyable and shared: a decoder holds one of these by value, and that must not be a
    // copy of the device itself.
    const DecodeDevice copy = cpu;
    CHECK(copy.kind() == cpu.kind());

    // An empty preference list under Auto is the caller's mistake, not something to guess
    // at: the library does not know whether an integrated engine or a discrete card is
    // wanted here.
    CHECK(!DecodeDevice::Opened(DecodeOrder{}).ok());
}

} // namespace

int main()
{
    a_whole_access_unit_decodes_to_a_picture_without_the_next_one();
    a_cpu_frame_is_already_readable_and_downloads_to_itself();
    the_cpu_device_is_a_value_rather_than_an_absence();
    std::printf("decoder_test ok\n");
    return 0;
}
