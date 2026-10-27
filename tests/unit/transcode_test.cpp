// Transcoder fed compressed input, on the software path: no GPU.
//
// What the caller hands submit() is a span over its own memory, sized to the access unit and
// nothing more -- a std::vector, not a demuxer's packet with zeroed bytes after it. FFmpeg's
// parser reads past the end of what it is given, so the library has to give it something it
// may read past; under AddressSanitizer this test is what says it does.
//
// And every access unit submitted comes out as a frame once finish() returns. The parser in
// front of an Annex-B decode holds each access unit until the next one begins, so the last one
// is still inside it when the input stops; finish() is what has to get it out. A frame short
// is invisible on a live stream and the last frame of every recompressed clip.

#include "sample_stream.h"

#include "ta_roienc/access_unit_sink.h"
#include "ta_roienc/transcoder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
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
constexpr int64_t kFrames = 12;

class Collecting final : public AccessUnitSink {
public:
    void on_access_unit(const AccessUnit& unit) override { pts.push_back(unit.pts); }
    std::vector<int64_t> pts;
};

void every_access_unit_submitted_comes_out_as_a_frame()
{
    // One vector per access unit, each exactly as long as its payload.
    const std::vector<std::vector<uint8_t>> units =
        sample_stream::annex_b_h264(kWidth, kHeight, kFrames);

    TranscoderOptions p;
    p.width = kWidth;
    p.height = kHeight;
    p.time_base_num = 1;
    p.time_base_den = 25;
    // No extradata: the parameter sets are in band, which is the shape that puts a parser in
    // front of the decoder.
    p.codec = SourceCodec::H264;
    p.settings.encoder = Engine::X264;
    p.settings.codec = Coding::H264;
    p.settings.metadata.bitstream_sei = false;

    Collecting sink;
    Transcoder session(p, &sink);
    for (std::size_t i = 0; i < units.size(); ++i) {
        session.submit(std::span<const uint8_t>(units[i]), Regions{}, static_cast<int64_t>(i));
    }
    const Outcome r = session.finish();

    CHECK(r.frames == kFrames);
    CHECK(static_cast<int64_t>(sink.pts.size()) == kFrames);
    // Each frame keeps the timestamp of the access unit it was decoded from, the last one
    // included. The sample has no B-frames, so output order is input order.
    for (std::size_t i = 0; i < sink.pts.size(); ++i) {
        CHECK(sink.pts[i] == static_cast<int64_t>(i));
    }
}

} // namespace

int main()
{
    every_access_unit_submitted_comes_out_as_a_frame();
    std::printf("transcode_test ok\n");
    return 0;
}
