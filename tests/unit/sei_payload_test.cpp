// SEI payload round-trip -- TZ v3 §19.2 (no codec).

#include "sve/metadata.h"

#include <cstdio>
#include <cstring>
#include <string>

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

int main()
{
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
