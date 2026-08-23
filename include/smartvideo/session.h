#pragma once

// Frames in, encoded packets out -- for a caller that has no file to hand over.

#include "smartvideo/frame_buffer.h"
#include "smartvideo/geometry.h"
#include "smartvideo/memory.h"
#include "smartvideo/packet_handler.h"
#include "smartvideo/report.h"
#include "smartvideo/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace smartvideo {

// What to keep sharp in this frame: a mask, or boxes, or neither.
//
// Both carry their own length. The previous shape was a pointer and a count the caller
// filled in separately, which meant a wrong count read past the end of their buffer and
// nothing here could tell.
struct Regions {
    /// Takes precedence over `boxes` when it holds a consistent mask.
    Mask mask;
    std::span<const Box> boxes;
};

struct SessionParams {
    int32_t width = 0;
    int32_t height = 0;
    // Units of the `pts` passed to submit().
    int32_t time_base_num = 1;
    int32_t time_base_den = 1000;
    // Only sets the encoder's frame rate and the default GOP.
    int32_t fps_num = 25;
    int32_t fps_den = 1;

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

    // One frame with its regions, in whichever form the caller has it. A frame carrying
    // neither a mask nor boxes is encoded with the background treatment alone.
    //
    // Two overloads rather than one struct with three optional shapes in it. The struct
    // could be filled wrongly in ways nothing could detect -- planes and an access unit at
    // once, or neither -- and every caller paid for that ambiguity by reading submit() to
    // find out which field mattered. There is nothing here that can be null.
    //
    // `pixels` is for a session opened with InputCodec::Raw; wrap planes you already have
    // in a PlaneBuffer, or take a buffer from the encoder's own memory.
    void submit(IFrameBuffer& pixels, const Regions& regions, int64_t pts);

    // `access_unit` is for a session opened with a codec: one compressed frame, decoded
    // here and re-encoded with the regions applied.
    void submit(std::span<const uint8_t> access_unit, const Regions& regions, int64_t pts);

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

} // namespace smartvideo
