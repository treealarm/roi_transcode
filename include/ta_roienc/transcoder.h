#pragma once

// Frames in, encoded packets out -- for a caller that has no file to hand over.

#include "ta_roienc/picture_store.h"
#include "ta_roienc/picture_info.h"
#include "ta_roienc/shapes.h"
#include "ta_roienc/memory_kind.h"
#include "ta_roienc/access_unit_sink.h"
#include "ta_roienc/outcome.h"
#include "ta_roienc/basics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ta_roienc {

struct TranscoderOptions {
    int32_t width = 0;
    int32_t height = 0;
    // Units of the `pts` passed to submit().
    int32_t time_base_num = 1;
    int32_t time_base_den = 1000;
    // Only sets the encoder's frame rate and the default GOP.
    int32_t fps_num = 25;
    int32_t fps_den = 1;

    SourceCodec codec = SourceCodec::Raw;
    // Codec configuration for AVCC/HVCC input; empty for Annex-B or raw.
    std::vector<uint8_t> extradata;

    Recipe settings;

    // When set, packets are muxed into this file. Leave empty to receive them
    // through a AccessUnitSink instead; giving neither is an error.
    std::string output_path;
};

class Transcoder {
public:
    // `handler` may be null when params.output_path names a file.
    explicit Transcoder(const TranscoderOptions& params, AccessUnitSink* handler = nullptr);
    ~Transcoder();
    Transcoder(const Transcoder&) = delete;
    Transcoder& operator=(const Transcoder&) = delete;

    // One frame with its regions, in whichever form the caller has it. A frame carrying
    // neither a mask nor boxes is encoded with the background treatment alone.
    //
    // Two overloads rather than one struct with three optional shapes in it. The struct
    // could be filled wrongly in ways nothing could detect -- planes and an access unit at
    // once, or neither -- and every caller paid for that ambiguity by reading submit() to
    // find out which field mattered. There is nothing here that can be null.
    //
    // `pixels` is for a session opened with SourceCodec::Raw; wrap planes you already have
    // in a LayerStore, or take a buffer from the encoder's own memory.
    void submit(IPictureStore& pixels, const Regions& regions, int64_t pts);

    // `access_unit` is for a session opened with a codec: one compressed frame, decoded
    // here and re-encoded with the regions applied. Only the bytes of the span are read --
    // no padding is needed after them, whatever FFmpeg asks of its own callers -- and none
    // is kept after the call returns.
    void submit(std::span<const uint8_t> access_unit, const Regions& regions, int64_t pts);

    // Which backend opened. Known as soon as the session exists, and with
    // Engine::Auto that is the only way to find out.
    Engine engine_used() const;
    std::string engine_name() const;

    // The hardware the decode is running on ("vaapi", "qsv"), empty when it runs on the CPU.
    //
    // Unlike the encoder's, this is not knowable when the session opens: libavcodec settles
    // the decoder's pixel format on the first packet, so before then there is an intention
    // and nothing more. Empty therefore means "on the CPU, or not asked yet" -- a live
    // caller reads it once frames are flowing, and finish() reports the settled answer.
    std::string decoder_hardware() const;

    // Drains decoder and encoder, ends the sink, and reports what happened. Every access
    // unit submitted is decoded by then, the last one included.
    Outcome finish();

private:
    struct Internals;
    std::unique_ptr<Internals> m_impl;
};

} // namespace ta_roienc
