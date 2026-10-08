// The NDJSON sidecar record.
//
// By hand, and for the same reason src/metadata/cbor.cpp is: the core links nothing, and
// this is one fixed schema rather than arbitrary JSON. A parser would be the larger half of
// a dependency we do not need.
//
// Numbers are written the way the SEI payload writes them into CBOR -- geometry and
// confidence as scaled integers rather than floats. Two encoders may pick different float
// widths and produce different payloads for the same data, and a float printed by one
// library and read by another is exactly where a "why does the last digit differ" bug
// comes from.

#include "ta_roienc/metadata.h"

#include <array>
#include <cstdio>
#include <string>

namespace ta_roienc {

namespace {

/// The six characters JSON requires escaped, plus the control range. Anything else is
/// passed through: the caller's class names are their own, and mangling them here would be
/// a surprise nobody asked for.
void append_escaped(std::string& out, const std::string& text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\u%04x",
                    static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf.data();
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

/// Normalized geometry as an integer per ten-thousandth of the frame, and confidence per
/// ten-thousandth of one. The same scale the SEI payload uses, so a reader of one channel
/// reads the other without a second convention to learn.
int64_t scaled(float value)
{
    return static_cast<int64_t>(value * 10000.0f + (value < 0 ? -0.5f : 0.5f));
}

void append_number(std::string& out, int64_t value)
{
    out += std::to_string(value);
}

} // namespace

std::string encode_sidecar_ndjson(const SeiFrame& frame, uint64_t frame_index,
    uint32_t max_objects)
{
    std::string out;
    out.reserve(64 + frame.objects.size() * 96);
    out += R"({"frame_index":)";
    append_number(out, static_cast<int64_t>(frame_index));
    out += R"(,"frame_id":)";
    append_number(out, static_cast<int64_t>(frame.frame_id));
    out += R"(,"pts":)";
    append_number(out, frame.pts);
    out += R"(,"objects":[)";

    // The same cap the SEI payload applies, so the two channels describe one frame the
    // same way rather than disagreeing about how many objects it had.
    std::size_t written = 0;
    for (const SeiObject& o : frame.objects) {
        if (max_objects > 0 && written >= max_objects) {
            break;
        }
        if (written > 0) {
            out += ',';
        }
        out += R"({"track_id":)";
        append_number(out, static_cast<int64_t>(o.track_id));
        out += R"(,"x":)";       append_number(out, scaled(o.x));
        out += R"(,"y":)";       append_number(out, scaled(o.y));
        out += R"(,"width":)";   append_number(out, scaled(o.width));
        out += R"(,"height":)";  append_number(out, scaled(o.height));
        out += R"(,"confidence":)"; append_number(out, scaled(o.confidence));
        out += R"(,"priority":)";
        append_number(out, static_cast<int64_t>(o.priority));
        out += R"(,"class":)";
        append_escaped(out, o.class_name);
        if (o.has_desired_qp_delta) {
            out += R"(,"desired_qp_delta":)";
            append_number(out, o.desired_qp_delta);
        }
        out += '}';
        ++written;
    }
    out += "]}";
    return out;
}

} // namespace ta_roienc
