#include "sve/metadata.h"
#include "sve/types.h"

#include <algorithm>
#include <cstring>

namespace sve {

namespace {

// "sve-roimeta\0\0\0\1" -- stable id for this schema, not a random UUID.
constexpr uint8_t kUuid[16] = {
    's', 'v', 'e', '-', 'r', 'o', 'i', 'm', 'e', 't', 'a', 0, 0, 0, 1};

constexpr uint16_t kVersion = 1;
constexpr char kMagic[4] = {'S', 'V', 'E', '1'};

// IEEE CRC-32 (reflected), poly 0xEDB88320.
uint32_t crc32(std::span<const uint8_t> data)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        c ^= b;
        for (int i = 0; i < 8; ++i) {
            const uint32_t mask = -(c & 1u);
            c = (c >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~c;
}

void put_u16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void put_u64(std::vector<uint8_t>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void put_i64(std::vector<uint8_t>& out, int64_t v)
{
    put_u64(out, static_cast<uint64_t>(v));
}

void put_f32(std::vector<uint8_t>& out, float v)
{
    uint32_t bits = 0;
    static_assert(sizeof(float) == 4);
    std::memcpy(&bits, &v, 4);
    put_u32(out, bits);
}

bool read_u16(std::span<const uint8_t>& in, uint16_t& v)
{
    if (in.size() < 2) {
        return false;
    }
    v = static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
    in = in.subspan(2);
    return true;
}

bool read_u32(std::span<const uint8_t>& in, uint32_t& v)
{
    if (in.size() < 4) {
        return false;
    }
    v = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8)
        | (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
    in = in.subspan(4);
    return true;
}

bool read_u64(std::span<const uint8_t>& in, uint64_t& v)
{
    if (in.size() < 8) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    in = in.subspan(8);
    return true;
}

bool read_i64(std::span<const uint8_t>& in, int64_t& v)
{
    uint64_t u = 0;
    if (!read_u64(in, u)) {
        return false;
    }
    v = static_cast<int64_t>(u);
    return true;
}

bool read_f32(std::span<const uint8_t>& in, float& v)
{
    uint32_t bits = 0;
    if (!read_u32(in, bits)) {
        return false;
    }
    std::memcpy(&v, &bits, 4);
    return true;
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
        // Keep strongest protection first (Critical before Background).
        std::stable_sort(objs.begin(), objs.end(),
            [](const SeiObject& a, const SeiObject& b) {
                return priority_rank(a.priority) > priority_rank(b.priority);
            });
        objs.resize(max_objects);
    }

    std::vector<uint8_t> out;
    out.reserve(64 + objs.size() * 48);
    out.insert(out.end(), kMagic, kMagic + 4);
    put_u16(out, kVersion);
    put_u16(out, 0); // flags
    put_u64(out, frame.frame_id);
    put_i64(out, frame.pts);
    put_u32(out, static_cast<uint32_t>(objs.size()));

    for (const SeiObject& o : objs) {
        put_u64(out, o.track_id);
        put_f32(out, o.x);
        put_f32(out, o.y);
        put_f32(out, o.width);
        put_f32(out, o.height);
        put_f32(out, o.confidence);
        out.push_back(static_cast<uint8_t>(o.priority));
        out.push_back(o.has_desired_qp_delta ? 1 : 0);
        out.push_back(0);
        out.push_back(0);
        put_u32(out, static_cast<uint32_t>(o.desired_qp_delta));
        std::string name = o.class_name;
        if (name.size() > 32) {
            name.resize(32);
        }
        put_u16(out, static_cast<uint16_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }

    const uint32_t sum = crc32(out);
    put_u32(out, sum);
    return out;
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
    if (bytes.size() < 4 + 2 + 2 + 8 + 8 + 4 + 4) {
        return false;
    }
    const uint32_t expect = crc32(bytes.first(bytes.size() - 4));
    uint32_t got = 0;
    auto crc_span = bytes.subspan(bytes.size() - 4);
    if (!read_u32(crc_span, got) || got != expect) {
        return false;
    }
    bytes = bytes.first(bytes.size() - 4);

    if (bytes.size() < 4 || bytes[0] != 'S' || bytes[1] != 'V' || bytes[2] != 'E'
        || bytes[3] != '1') {
        return false;
    }
    bytes = bytes.subspan(4);
    uint16_t version = 0;
    uint16_t flags = 0;
    if (!read_u16(bytes, version) || !read_u16(bytes, flags) || version != kVersion) {
        return false;
    }
    (void)flags;
    if (!read_u64(bytes, out_frame.frame_id) || !read_i64(bytes, out_frame.pts)) {
        return false;
    }
    uint32_t count = 0;
    if (!read_u32(bytes, count)) {
        return false;
    }
    out_frame.objects.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SeiObject o;
        uint32_t qp_bits = 0;
        uint16_t name_len = 0;
        uint8_t prio = 0;
        uint8_t has_qp = 0;
        uint8_t pad0 = 0;
        uint8_t pad1 = 0;
        if (!read_u64(bytes, o.track_id) || !read_f32(bytes, o.x) || !read_f32(bytes, o.y)
            || !read_f32(bytes, o.width) || !read_f32(bytes, o.height)
            || !read_f32(bytes, o.confidence)) {
            return false;
        }
        if (bytes.size() < 4) {
            return false;
        }
        prio = bytes[0];
        has_qp = bytes[1];
        pad0 = bytes[2];
        pad1 = bytes[3];
        (void)pad0;
        (void)pad1;
        bytes = bytes.subspan(4);
        if (!read_u32(bytes, qp_bits) || !read_u16(bytes, name_len)) {
            return false;
        }
        if (bytes.size() < name_len) {
            return false;
        }
        o.priority = static_cast<ObjectPriority>(prio);
        o.has_desired_qp_delta = has_qp != 0;
        o.desired_qp_delta = static_cast<int32_t>(qp_bits);
        o.class_name.assign(reinterpret_cast<const char*>(bytes.data()), name_len);
        bytes = bytes.subspan(name_len);
        out_frame.objects.push_back(std::move(o));
    }
    return bytes.empty();
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
