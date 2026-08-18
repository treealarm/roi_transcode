#pragma once

// Frames in, encoded packets out. This is the entry point for a project that
// has no file to hand over: it pushes frames -- raw or already compressed --
// each with the regions worth keeping sharp, and takes packets back.

#include "roi/geometry.h"
#include "roi/roi_types.h"
#include "roi/transcoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class PacketSink;

namespace roi {

enum class InputCodec { Raw, H264, H265, Jpeg };

// One frame on its way in: either raw planes or one encoded access unit.
struct Frame {
    // Raw form. `pix_fmt` is an AVPixelFormat value; planes are not copied.
    const uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr};
    int stride[4] = {0, 0, 0, 0};
    int pix_fmt = -1;
    int width = 0;
    int height = 0;

    // Compressed form, used when the session was opened with a codec.
    const uint8_t* encoded = nullptr;
    std::size_t encoded_size = 0;

    // In the session's time base.
    int64_t pts = 0;
};

// What to keep sharp in this frame: a mask, or boxes, or neither.
struct Regions {
    const RoiMask* mask = nullptr;
    const RoiSample* boxes = nullptr;
    std::size_t box_count = 0;
};

// Per-kind quality for box input. Masks carry their own strength, so this
// applies only to boxes.
struct BoxQuality {
    std::unordered_map<std::string, KindConfig> kinds;
    // 0 leaves the background alone rather than meaning "unset".
    int background_qp_delta = 6;
};

struct SessionParams {
    int width = 0;
    int height = 0;
    // Units of Frame::pts.
    int time_base_num = 1;
    int time_base_den = 1000;
    // Only used for the encoder's frame rate and the default GOP.
    int fps_num = 25;
    int fps_den = 1;

    InputCodec codec = InputCodec::Raw;
    std::vector<uint8_t> extradata;

    EncoderKind encoder = EncoderKind::Auto;
    int crf = 28;
    int gop = 0;
    std::string preset = "medium";

    MaskMapping mask_mapping;
    BoxQuality box_quality;
    int max_regions = 32;
};

struct Report {
    EncoderKind encoder_used = EncoderKind::X265;
    std::string encoder_name;
    int64_t frames = 0;
    int64_t packets = 0;
    int64_t bytes = 0;
    // Regions the encoder's region budget could not take.
    int64_t regions_dropped = 0;
};

class Session {
public:
    Session(const SessionParams& params, PacketSink& sink);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // One frame with its regions. A frame carrying neither a mask nor boxes is
    // encoded with the background treatment alone.
    void submit(const Frame& frame, const Regions& regions);

    // Drains decoder and encoder, ends the sink, and reports what happened.
    Report finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace roi
