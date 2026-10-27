// A hardware encoder fed by the decoder in front of it: the frames have to stay on the GPU.
//
// Transcoder hands its decoder the device the encoder names (VideoEncoder::HardwareDevice).
// For VA-API and NVENC that is the encoder's own; for QSV it is the device QSV runs on top of,
// because no decoder decodes into QSV surfaces, and the encoder maps every decoded frame into
// its own pool. Both halves go wrong quietly -- the decode falls back to the CPU, or each frame
// comes down to system memory and goes up again -- and the stream still comes out right. So
// what is checked is the cost -- the decoder reports hardware and nothing was uploaded --
// and then the picture: each frame decoded back from the output has its edge where that
// frame's input had it, which a frame mapped from the wrong surface would not.
//
// Needs the hardware. A backend that will not open on this machine is skipped and says why;
// one that opens has to pass.

#include "sample_stream.h"

#include "ta_roienc/access_unit_sink.h"
#include "ta_roienc/decoder.h"
#include "ta_roienc/transcoder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <span>
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

// Large enough for every hardware encoder's minimum picture size, and a multiple of the
// 16-pixel macroblock so that no backend pads it.
constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 240;
constexpr int64_t kFrames = 30;

/// How far from the edge a pixel is read, on each side: past the ringing a coarse
/// quantiser leaves along it.
constexpr int64_t kEdgeMargin = 4;

class Collecting final : public AccessUnitSink {
public:
    void on_start(const SourceInfo& info) override
    {
        extradata.assign(info.extradata.begin(), info.extradata.end());
    }
    void on_access_unit(const AccessUnit& unit) override
    {
        const std::span<const uint8_t> bytes = unit.payload.bytes();
        units.emplace_back(bytes.begin(), bytes.end());
    }
    std::vector<uint8_t> extradata;
    std::vector<std::vector<uint8_t>> units;
};

/// Whether a decoded frame shows frame `index` of the sample: dark just left of its edge and
/// bright just right of it, read on the middle row.
bool shows_frame(const Picture& frame, int64_t index)
{
    const LayerView luma = frame.shared()->plane(0);
    const std::span<uint8_t> row = luma.row(kHeight / 2);
    const int64_t edge = sample_stream::edge_column(kWidth, index);
    constexpr int kMiddle = (sample_stream::kDark + sample_stream::kBright) / 2;
    const auto at = [&row](int64_t col) { return static_cast<int>(row[static_cast<std::size_t>(col)]); };
    const int64_t dark = edge - kEdgeMargin;
    const int64_t bright = std::max<int64_t>(edge + kEdgeMargin, 0);
    return (dark < 0 || at(dark) < kMiddle) && (bright >= kWidth || at(bright) > kMiddle);
}

/// Decodes the session's output on the CPU and counts the frames that show the right picture.
int64_t frames_in_place(const Collecting& out)
{
    VideoDecoder decoder;
    CHECK(decoder.Opened(SourceCodec::H264, out.extradata, DecodeDevice::Cpu()).ok());
    int64_t index = 0;
    int64_t right = 0;
    const auto check = [&](const Picture& frame, const DecodedPictureInfo&) {
        right += shows_frame(frame, index) ? 1 : 0;
        ++index;
    };
    for (const std::vector<uint8_t>& unit : out.units) {
        CHECK(decoder.Push(unit, Stamp{index, Ratio{1, 25}}, check).ok());
    }
    CHECK(decoder.Flush(check).ok());
    CHECK(index == static_cast<int64_t>(out.units.size()));
    return right;
}

void stays_on_the_gpu(Engine engine, const char* name,
    const std::vector<std::vector<uint8_t>>& input)
{
    TranscoderOptions p;
    p.width = kWidth;
    p.height = kHeight;
    p.time_base_num = 1;
    p.time_base_den = 25;
    p.codec = SourceCodec::H264;
    p.settings.encoder = engine;
    p.settings.codec = Coding::H264;
    // No hardware backend carries SEI, and asking would be answered with a fallback.
    p.settings.metadata.bitstream_sei = false;

    Collecting sink;
    std::unique_ptr<Transcoder> session;
    try {
        session = std::make_unique<Transcoder>(p, &sink);
    } catch (const std::exception& e) {
        std::printf("  skip  %-8s %s\n", name, e.what());
        return;
    }
    CHECK(session->engine_used() == engine);

    for (std::size_t i = 0; i < input.size(); ++i) {
        session->submit(input[i], Regions{}, static_cast<int64_t>(i));
    }
    const Outcome r = session->finish();

    const int64_t submitted = static_cast<int64_t>(input.size());
    const int64_t units = static_cast<int64_t>(sink.units.size());
    const int64_t in_place = frames_in_place(sink);
    std::printf("  %-4s  %-8s decoded on '%s', %lld frames, %lld uploaded, %lld converted, "
                "%lld units, %lld pictures right\n",
        !r.decoder_hardware.empty() && r.frames_uploaded == 0 && units == submitted
                && in_place == units
            ? "ok"
            : "FAIL",
        name, r.decoder_hardware.c_str(), static_cast<long long>(r.frames),
        static_cast<long long>(r.frames_uploaded), static_cast<long long>(r.frames_converted),
        static_cast<long long>(units), static_cast<long long>(in_place));
    // Every frame submitted came out, which is what says the mapped frames encoded rather
    // than being dropped.
    CHECK(r.frames == submitted);
    CHECK(units == submitted);
    CHECK(in_place == units);
    CHECK(!r.decoder_hardware.empty());
    CHECK(r.frames_uploaded == 0);
    CHECK(r.frames_converted == 0);
}

} // namespace

int main()
{
    const std::vector<std::vector<uint8_t>> units =
        sample_stream::annex_b_h264(kWidth, kHeight, kFrames);
    stays_on_the_gpu(Engine::IntelOneVpl, "qsv", units);
    stays_on_the_gpu(Engine::Vaapi, "vaapi", units);
    stays_on_the_gpu(Engine::Nvenc, "nvenc", units);
    return 0;
}
