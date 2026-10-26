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

#include "enum_names.h"
#include "sample_stream.h"

#include "ta_roienc/decoder.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"
#include "ta_roienc/access_unit.h"
#include "ta_roienc/picture_view.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
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

/// kFrames access units of H.264, in encode order and one to a vector.
std::vector<std::vector<uint8_t>> annex_b_stream()
{
    return sample_stream::annex_b_h264(kWidth, kHeight, kFrames);
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

// Here rather than in labels_test, which links without the decoder when it is not built.
// Every backend has a name of its own, the name parses back to a list holding that backend
// alone, and "auto" tries every one of them -- a backend left out of it is one "auto" quietly
// never reaches.
void every_decode_backend_is_named_and_reachable_from_auto()
{
    const DecodeOrder automatic = decode_order_from_name("auto");
    std::set<std::string_view> seen;
    ta_roienc_enum_table::for_each<DecodeEngine>([&](DecodeEngine value, const char*) {
        const std::string_view name = decode_backend_name(value);
        CHECK(!name.empty() && name != "?");
        CHECK(seen.insert(name).second);
        if (value == DecodeEngine::Auto) {
            return;
        }
        const DecodeOrder named = decode_order_from_name(name);
        CHECK(named.preferred.size() == 1 && named.preferred[0] == value);
        CHECK(std::ranges::find(automatic.preferred, value) != automatic.preferred.end());
    });
    CHECK(!seen.empty());
    // Software last, so that "auto" means "hardware if you have it".
    CHECK(automatic.preferred.back() == DecodeEngine::Cpu);
}

} // namespace

int main()
{
    a_whole_access_unit_decodes_to_a_picture_without_the_next_one();
    a_cpu_frame_is_already_readable_and_downloads_to_itself();
    the_cpu_device_is_a_value_rather_than_an_absence();
    every_decode_backend_is_named_and_reachable_from_auto();
    std::printf("decoder_test ok\n");
    return 0;
}
