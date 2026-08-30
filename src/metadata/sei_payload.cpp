#include "sve/metadata.h"

#include "cbor.h"
#include "sve/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sve {

namespace {

// "sve-roimeta\0\0\0\1" -- stable id for this schema, not a random UUID.
constexpr uint8_t kUuid[16] = {
    's', 'v', 'e', '-', 'r', 'o', 'i', 'm', 'e', 't', 'a', 0, 0, 0, 1};

/// Bumped only by an incompatible change. §19.2 requires a parser to refuse a version it
/// does not know rather than read what it can of it, so this is checked before anything
/// else in the payload.
constexpr uint64_t kVersion = 1;

/// §19.2: a frame's whole SEI payload stays under 16 KiB. Above it, objects are dropped
/// from the least important upward, and the fact is reported to the caller.
constexpr std::size_t kMaxPayloadBytes = 16 * 1024;

/// CRC-32C (Castagnoli), polynomial 0x1EDC6F41 in the reflected form 0x82F63B78 that a
/// table-free implementation uses. §19.2 names this one specifically, and it is not the
/// CRC-32 of zip and PNG that this payload used to carry.
uint32_t crc32c(std::span<const uint8_t> data)
{
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t b : data) {
        c ^= b;
        for (int i = 0; i < 8; ++i) {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(c & 1u));
            c = (c >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~c;
}

/// Geometry and confidence travel as scaled integers rather than floats.
///
/// CBOR has three float widths and choosing between them is the encoder's business, so two
/// encoders that choose differently produce payloads that differ without meaning to. This
/// payload is checksummed, which turns "differs without meaning to" into a rejected frame.
/// A normalised coordinate to four decimal places is finer than any block grid can act on.
constexpr float kGeometryScale = 10000.0f;
constexpr float kConfidenceScale = 1000.0f;

int64_t scaled(float value, float scale)
{
    return static_cast<int64_t>(std::lround(value * scale));
}

/// One object, as a fixed-order CBOR array.
///
/// An array rather than a map: the keys would be the larger half of the payload, and §19.2
/// caps what a frame may spend. The order is the schema, and `version` is what allows it to
/// change.
///
/// No mask data, by §19.2 -- only whether the object had one.
void put_object(std::vector<uint8_t>& out, const SeiObject& o)
{
    cbor::put_array(out, 9);
    cbor::put_uint(out, o.track_id);
    cbor::put_int(out, scaled(o.x, kGeometryScale));
    cbor::put_int(out, scaled(o.y, kGeometryScale));
    cbor::put_int(out, scaled(o.width, kGeometryScale));
    cbor::put_int(out, scaled(o.height, kGeometryScale));
    cbor::put_int(out, scaled(o.confidence, kConfidenceScale));
    cbor::put_uint(out, static_cast<uint64_t>(o.priority));
    cbor::put_int(out, o.has_desired_qp_delta ? o.desired_qp_delta : 0);
    std::string name = o.class_name;
    if (name.size() > 32) {
        name.resize(32);
    }
    cbor::put_text(out, name);
}

bool read_object(cbor::Reader& r, SeiObject& o)
{
    uint64_t fields = 0;
    if (!r.array(fields) || fields < 9) {
        return false;
    }
    int64_t x = 0, y = 0, w = 0, h = 0, conf = 0, qp = 0;
    uint64_t priority = 0;
    if (!r.uint(o.track_id) || !r.integer(x) || !r.integer(y) || !r.integer(w)
        || !r.integer(h) || !r.integer(conf) || !r.uint(priority) || !r.integer(qp)
        || !r.text(o.class_name)) {
        return false;
    }
    o.x = static_cast<float>(x) / kGeometryScale;
    o.y = static_cast<float>(y) / kGeometryScale;
    o.width = static_cast<float>(w) / kGeometryScale;
    o.height = static_cast<float>(h) / kGeometryScale;
    o.confidence = static_cast<float>(conf) / kConfidenceScale;
    o.priority = static_cast<ObjectPriority>(priority);
    o.desired_qp_delta = static_cast<int32_t>(qp);
    o.has_desired_qp_delta = qp != 0;
    // A newer writer may have added fields to the end. Skipping them keeps this reader
    // working against it, which is the half of versioning that is not the version number.
    for (uint64_t i = 9; i < fields; ++i) {
        if (!r.skip()) {
            return false;
        }
    }
    return true;
}

/// Everything but the checksum, so it can be taken over exactly these bytes.
std::vector<uint8_t> encode_body(const SeiFrame& frame, const std::vector<SeiObject>& objs)
{
    std::vector<uint8_t> out;
    out.reserve(32 + objs.size() * 48);
    cbor::put_array(out, 4);
    cbor::put_uint(out, kVersion);
    cbor::put_uint(out, frame.frame_id);
    cbor::put_int(out, frame.pts);
    cbor::put_array(out, objs.size());
    for (const SeiObject& o : objs) {
        put_object(out, o);
    }
    return out;
}

/// Least important first, so a cap removes the ones that matter least.
void order_by_priority(std::vector<SeiObject>& objs)
{
    std::stable_sort(objs.begin(), objs.end(),
        [](const SeiObject& a, const SeiObject& b) {
            return priority_rank(a.priority) > priority_rank(b.priority);
        });
}

} // namespace

std::span<const uint8_t, 16> sei_uuid()
{
    return std::span<const uint8_t, 16>{kUuid, 16};
}

std::vector<uint8_t> encode_sei_payload(const SeiFrame& frame, uint32_t max_objects)
{
    std::vector<SeiObject> objs = frame.objects;
    if (max_objects > 0 && objs.size() > max_objects) {
        order_by_priority(objs);
        objs.resize(max_objects);
    }

    std::vector<uint8_t> body = encode_body(frame, objs);

    // §19.2's byte cap, which the object count alone cannot enforce: a class name is up to
    // 32 bytes, so a frame within its object budget can still be over its byte budget.
    // Drop from the least important upward until it fits.
    if (body.size() + 4 > kMaxPayloadBytes && !objs.empty()) {
        order_by_priority(objs);
        while (!objs.empty() && body.size() + 4 > kMaxPayloadBytes) {
            objs.pop_back();
            body = encode_body(frame, objs);
        }
    }

    const uint32_t sum = crc32c(body);
    // The checksum is not part of the CBOR item: §19.2 puts it after all previous fields
    // of the payload, and a reader takes it from the tail before parsing anything.
    body.push_back(static_cast<uint8_t>(sum));
    body.push_back(static_cast<uint8_t>(sum >> 8));
    body.push_back(static_cast<uint8_t>(sum >> 16));
    body.push_back(static_cast<uint8_t>(sum >> 24));
    return body;
}

std::vector<uint8_t> encode_sei_side_data(const SeiFrame& frame, uint32_t max_objects)
{
    const auto payload = encode_sei_payload(frame, max_objects);
    std::vector<uint8_t> out;
    out.reserve(16 + payload.size());
    out.insert(out.end(), kUuid, kUuid + 16);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool decode_sei_payload(std::span<const uint8_t> bytes, SeiFrame& out_frame)
{
    out_frame = SeiFrame{};
    if (bytes.size() >= 16 && std::equal(bytes.begin(), bytes.begin() + 16, kUuid)) {
        bytes = bytes.subspan(16);
    }
    if (bytes.size() < 5) {
        return false;
    }

    const std::span<const uint8_t> body = bytes.first(bytes.size() - 4);
    const std::span<const uint8_t> tail = bytes.subspan(bytes.size() - 4);
    const uint32_t got = static_cast<uint32_t>(tail[0])
        | (static_cast<uint32_t>(tail[1]) << 8) | (static_cast<uint32_t>(tail[2]) << 16)
        | (static_cast<uint32_t>(tail[3]) << 24);
    if (got != crc32c(body)) {
        return false;
    }

    cbor::Reader r(body);
    uint64_t fields = 0;
    if (!r.array(fields) || fields < 4) {
        return false;
    }
    uint64_t version = 0;
    if (!r.uint(version) || version != kVersion) {
        // §19.2: an unknown version is refused rather than read as far as it goes.
        return false;
    }
    if (!r.uint(out_frame.frame_id) || !r.integer(out_frame.pts)) {
        return false;
    }
    uint64_t count = 0;
    if (!r.array(count)) {
        return false;
    }
    // The count is the writer's claim; the reader still has to find that many objects in
    // the bytes it actually holds, and read_object is what enforces that.
    out_frame.objects.reserve(static_cast<std::size_t>(std::min<uint64_t>(count, 4096)));
    for (uint64_t i = 0; i < count; ++i) {
        SeiObject o;
        if (!read_object(r, o)) {
            return false;
        }
        out_frame.objects.push_back(std::move(o));
    }
    for (uint64_t i = 4; i < fields; ++i) {
        if (!r.skip()) {
            return false;
        }
    }
    return r.done();
}

std::vector<SeiObject> sei_objects_from_boxes(std::span<const Box> boxes, uint32_t max_objects)
{
    std::vector<SeiObject> out;
    out.reserve(boxes.size());
    for (const Box& b : boxes) {
        SeiObject o;
        o.track_id = b.track_id;
        o.x = b.x;
        o.y = b.y;
        o.width = b.width;
        o.height = b.height;
        o.confidence = b.confidence > 0.0f ? b.confidence : 1.0f;
        o.priority = b.has_priority ? b.priority : ObjectPriority::Normal;
        o.class_name = b.kind;
        o.has_desired_qp_delta = b.has_desired_qp_delta;
        o.desired_qp_delta = b.desired_qp_delta;
        out.push_back(std::move(o));
    }
    if (max_objects > 0 && out.size() > max_objects) {
        std::stable_sort(out.begin(), out.end(),
            [](const SeiObject& a, const SeiObject& b) {
                return priority_rank(a.priority) > priority_rank(b.priority);
            });
        out.resize(max_objects);
    }
    return out;
}

} // namespace sve
