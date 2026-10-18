#pragma once

// Bitstream object metadata helpers.
//
// Bitstream form is H.264/H.265 "user data unregistered" SEI: a fixed 16-byte UUID naming
// this library, then a versioned CBOR payload (frame_id, pts, objects) closed by a CRC-32C.
// Soft encoders (libx264/libx265) consume it via AV_FRAME_DATA_SEI_UNREGISTERED when
// `udu_sei` is on. Hardware backends that cannot insert SEI report capabilities.sei false;
// these helpers still work for tests and sidecars.
//
// Knobs live on MetadataSetup in rules.h (next to RoiPersistence).

#include "ta_roienc/salience.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ta_roienc {

struct Box;

/// One object in the SEI / EncodedFrameMetadata sense.
struct SeiItem {
    uint64_t track_id = 0;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float confidence = 1.0f;
    Salience priority = Salience::Normal;
    std::string class_name;
    bool has_desired_qp_delta = false;
    int32_t desired_qp_delta = 0;
};

struct SeiRecord {
    uint64_t frame_id = 0;
    int64_t pts = 0;
    std::vector<SeiItem> objects;
};

/// Our SEI UUID (16 bytes). Payload for FFmpeg is UUID || write_sei_record(...).
std::span<const uint8_t, 16> sei_identifier();

/// Versioned payload only (no UUID).
std::vector<uint8_t> write_sei_record(const SeiRecord& frame, uint32_t max_objects = 64);

/// UUID + payload -- what goes into AV_FRAME_DATA_SEI_UNREGISTERED.
std::vector<uint8_t> write_sei_side_data(const SeiRecord& frame, uint32_t max_objects = 64);

/// Decode a payload (with or without the leading UUID). Returns false on CRC/format error.
bool read_sei_record(std::span<const uint8_t> bytes, SeiRecord& out);

/// Build SeiItem list from public Box samples (normalized geometry).
std::vector<SeiItem> sei_objects_from_boxes(
    std::span<const Box> boxes, uint32_t max_objects = 64);

/// One NDJSON sidecar record. The same fields the SEI payload carries, plus `frame_index`,
/// which is the position of the packet this record accompanies.
///
/// Written by hand rather than through a JSON library: the core's dependency set is empty
/// and this is a fixed schema, not arbitrary JSON. No trailing newline -- the caller decides
/// what separates records, since a file wants one and a message queue does not.
std::string write_metadata_ndjson(const SeiRecord& frame, uint64_t frame_index,
    uint32_t max_objects = 64);

} // namespace ta_roienc
