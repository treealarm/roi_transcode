#pragma once

// A short H.264 stream for the tests that need compressed input, made by this library's own
// software encoder rather than read from a fixture: Annex-B with the parameter sets in band,
// one access unit per frame, and nothing checked in.

#include "ta_roienc/access_unit.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"
#include "ta_roienc/picture_view.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace sample_stream {

/// x264's quantiser for the sample: coarse, since what the tests look at is the stream's
/// shape and timing, not its picture.
constexpr int kCrf = 32;

/// The picture: dark left of a vertical edge, bright right of it, the edge moving left by
/// kEdgeStep pixels a frame from the middle. Mid-grey chroma.
constexpr uint8_t kDark = 40;
constexpr uint8_t kBright = 200;
constexpr uint8_t kGrey = 128;
constexpr int64_t kEdgeStep = 7;

/// The first bright column of frame `i`; negative once the edge has left the picture.
constexpr int64_t edge_column(int32_t width, int64_t i)
{
    return width / 2 - i * kEdgeStep;
}

inline void require(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "fail making the sample stream: %s\n", what);
        std::exit(1);
    }
}

/// `frames` access units of H.264 at width x height and 25 fps, in encode order and one to a
/// vector.
inline std::vector<std::vector<uint8_t>> annex_b_h264(int32_t width, int32_t height, int64_t frames)
{
    using namespace ta_roienc;

    EncodeOptions cfg;
    cfg.input.width = width;
    cfg.input.height = height;
    cfg.input.frame_rate = Ratio{25, 1};
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.encoder = Engine::X264;
    cfg.settings.codec = Coding::H264;
    cfg.settings.crf = kCrf;
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
            require(s.ok(), "ReceiveAccessUnit");
            const std::span<const uint8_t> bytes = packet.payload.bytes();
            units.emplace_back(bytes.begin(), bytes.end());
        }
    };

    struct Planes {
        std::vector<uint8_t> y, u, v;
    };
    const auto luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto pixels = std::make_shared<Planes>(Planes{std::vector<uint8_t>(luma, kGrey),
        std::vector<uint8_t>(luma / 4, kGrey), std::vector<uint8_t>(luma / 4, kGrey)});
    for (int64_t i = 0; i < frames; ++i) {
        // A moving edge, so the encoder has something to code and no two access units are
        // the same size. A flat picture would still exercise the timing, but a stream whose
        // packets are all alike hides an off-by-one between them.
        for (int32_t row = 0; row < height; ++row) {
            const std::size_t start = static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
            for (int32_t col = 0; col < width; ++col) {
                pixels->y[start + static_cast<std::size_t>(col)]
                    = col < edge_column(width, i) ? kDark : kBright;
            }
        }

        PictureView view;
        view.format.width = width;
        view.format.height = height;
        view.format.pixel_layout = PixelLayout::YUV420P;
        view.layer_count = 3;
        view.planes[0] = {pixels->y.data(), width, pixels->y.size()};
        view.planes[1] = {pixels->u.data(), width / 2, pixels->u.size()};
        view.planes[2] = {pixels->v.data(), width / 2, pixels->v.size()};
        view.timestamp = Stamp{i, Ratio{1, 25}};
        view.owner = pixels;

        StampedPicture timed;
        require(WrapPicture(view, &timed.frame).ok(), "WrapPicture");
        timed.metadata.timestamp = view.timestamp;
        require(encoder->SubmitPicture(timed).ok(), "SubmitPicture");
        drain();
    }
    require(encoder->Flush().ok(), "Flush");
    drain();
    require(static_cast<int64_t>(units.size()) == frames, "one access unit per frame");
    return units;
}

} // namespace sample_stream
