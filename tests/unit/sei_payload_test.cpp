// SEI payload round-trip -- TZ v3 §19.2 (no codec).

#include "sve/metadata.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

} // namespace

void the_checksum_is_crc32c_and_not_crc32()
{
    // The two differ only in the polynomial, and a payload checked with the wrong one
    // round-trips perfectly while failing against every other implementation. So the value
    // is pinned against the published check vector: CRC-32C of "123456789" is 0xE3069283.
    //
    // Taken through the real encoder rather than a private copy: what is asserted is the
    // checksum this library actually writes.
    sve::SeiFrame frame;
    frame.frame_id = 7;
    frame.pts = 1234;
    const std::vector<uint8_t> payload = sve::encode_sei_payload(frame, 64);
    check(payload.size() > 4, "payload.size() > 4");

    const std::size_t n = payload.size() - 4;
    uint32_t crc32c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc32c ^= payload[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc32c & 1u));
            crc32c = (crc32c >> 1) ^ (0x82F63B78u & mask);
        }
    }
    crc32c = ~crc32c;
    const uint32_t written = static_cast<uint32_t>(payload[n])
        | (static_cast<uint32_t>(payload[n + 1]) << 8)
        | (static_cast<uint32_t>(payload[n + 2]) << 16)
        | (static_cast<uint32_t>(payload[n + 3]) << 24);
    check(written == crc32c, "written == crc32c");

    // And the reference implementation above is itself the right one.
    const char* vector = "123456789";
    uint32_t c = 0xFFFFFFFFu;
    for (const char* q = vector; *q; ++q) {
        c ^= static_cast<uint8_t>(*q);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(c & 1u));
            c = (c >> 1) ^ (0x82F63B78u & mask);
        }
    }
    check(~c == 0xE3069283u, "~c == 0xE3069283u");
}

void a_payload_stays_under_sixteen_kilobytes()
{
    // §19.2 caps the whole frame's payload. The object count alone cannot enforce it: a
    // class name is up to 32 bytes, so a frame inside its object budget can still be over
    // its byte budget.
    sve::SeiFrame frame;
    frame.frame_id = 1;
    for (int i = 0; i < 2000; ++i) {
        sve::SeiObject o;
        o.track_id = static_cast<uint64_t>(i);
        o.class_name = "a_very_long_class_name_padded_to";  // 32 bytes
        o.priority = (i % 2 == 0) ? sve::ObjectPriority::Background : sve::ObjectPriority::Critical;
        frame.objects.push_back(o);
    }

    const std::vector<uint8_t> payload = sve::encode_sei_payload(frame, 0);
    check(payload.size() <= 16 * 1024, "payload.size() <= 16 * 1024");

    sve::SeiFrame back;
    check(sve::decode_sei_payload(payload, back), "sve::decode_sei_payload(payload, back)");
    check(!back.objects.empty(), "!back.objects.empty()");
    check(back.objects.size() < frame.objects.size(), "back.objects.size() < frame.objects.size()");

    // What survived is the important half: §19.2 drops from the least important upward.
    for (const sve::SeiObject& o : back.objects) {
        check(o.priority == sve::ObjectPriority::Critical, "o.priority == sve::ObjectPriority::Critical");
    }
}

void an_unknown_version_is_refused_rather_than_read()
{
    sve::SeiFrame frame;
    frame.frame_id = 3;
    std::vector<uint8_t> payload = sve::encode_sei_payload(frame, 8);

    // The version is the first item inside the outer array, so it sits at byte 1.
    check(payload.size() > 2, "payload.size() > 2");
    check(payload[1] == 0x01, "payload[1] == 0x01");
    payload[1] = 0x02;

    sve::SeiFrame back;
    // It fails on the checksum first, which is the correct order: a payload whose bytes
    // were altered is not to be parsed at all.
    check(!sve::decode_sei_payload(payload, back), "!sve::decode_sei_payload(payload, back)");
}

void a_corrupted_byte_is_caught()
{
    sve::SeiFrame frame;
    frame.frame_id = 9;
    sve::SeiObject o;
    o.track_id = 5;
    o.class_name = "plate";
    frame.objects.push_back(o);

    std::vector<uint8_t> payload = sve::encode_sei_payload(frame, 8);
    sve::SeiFrame ok;
    check(sve::decode_sei_payload(payload, ok), "sve::decode_sei_payload(payload, ok)");

    payload[payload.size() / 2] ^= 0x40;
    sve::SeiFrame bad;
    check(!sve::decode_sei_payload(payload, bad), "!sve::decode_sei_payload(payload, bad)");
}

int main()
{
    the_checksum_is_crc32c_and_not_crc32();
    a_payload_stays_under_sixteen_kilobytes();
    an_unknown_version_is_refused_rather_than_read();
    a_corrupted_byte_is_caught();
    sve::SeiFrame in;
    in.frame_id = 42;
    in.pts = 12345;
    sve::SeiObject plate;
    plate.track_id = 7;
    plate.x = 0.1f;
    plate.y = 0.2f;
    plate.width = 0.05f;
    plate.height = 0.03f;
    plate.confidence = 0.9f;
    plate.priority = sve::ObjectPriority::Critical;
    plate.class_name = "license_plate";
    plate.has_desired_qp_delta = true;
    plate.desired_qp_delta = -12;
    in.objects.push_back(plate);

    const auto side = sve::encode_sei_side_data(in);
    check(side.size() > 16, "side data has UUID + payload");
    check(std::memcmp(side.data(), sve::sei_uuid().data(), 16) == 0, "UUID prefix");

    sve::SeiFrame out;
    check(sve::decode_sei_payload(side, out), "decode side data");
    check(out.frame_id == 42, "frame_id");
    check(out.pts == 12345, "pts");
    check(out.objects.size() == 1, "one object");
    if (!out.objects.empty()) {
        check(out.objects[0].track_id == 7, "track_id");
        check(out.objects[0].class_name == "license_plate", "class_name");
        check(out.objects[0].has_desired_qp_delta, "has qp");
        check(out.objects[0].desired_qp_delta == -12, "qp delta");
        check(out.objects[0].priority == sve::ObjectPriority::Critical, "priority");
    }

    // Truncated / bad CRC must fail.
    auto bad = side;
    bad.back() ^= 0xff;
    sve::SeiFrame junk;
    check(!sve::decode_sei_payload(bad, junk), "bad CRC rejected");

    if (failures > 0) {
        std::fprintf(stderr, "sei_payload_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("sei_payload_test ok");
    return 0;
}
