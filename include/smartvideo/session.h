#pragma once

// Frames in, encoded packets out -- for a caller that has no file to hand over.

#include "smartvideo/geometry.h"
#include "smartvideo/packet_handler.h"
#include "smartvideo/report.h"
#include "smartvideo/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace roi {

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
    const Mask* mask = nullptr;
    const Box* boxes = nullptr;
    std::size_t box_count = 0;
};

struct SessionParams {
    int width = 0;
    int height = 0;
    // Units of Frame::pts.
    int time_base_num = 1;
    int time_base_den = 1000;
    // Only sets the encoder's frame rate and the default GOP.
    int fps_num = 25;
    int fps_den = 1;

    InputCodec codec = InputCodec::Raw;
    // Codec configuration for AVCC/HVCC input; empty for Annex-B or raw.
    std::vector<uint8_t> extradata;

    Settings settings;

    // When set, packets are muxed into this file. Leave empty to receive them
    // through a PacketHandler instead; giving neither is an error.
    std::string output_path;
};

class Session {
public:
    // `handler` may be null when params.output_path names a file.
    explicit Session(const SessionParams& params, PacketHandler* handler = nullptr);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // One frame with its regions. A frame carrying neither a mask nor boxes is
    // encoded with the background treatment alone.
    void submit(const Frame& frame, const Regions& regions);

    // Which backend opened. Known as soon as the session exists, and with
    // Encoder::Auto that is the only way to find out.
    Encoder encoder_used() const;
    std::string encoder_name() const;

    // Drains decoder and encoder, ends the sink, and reports what happened.
    Report finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace roi
