#include "cbor.h"

#include <cstring>

namespace sve::cbor {

namespace {

constexpr uint8_t kMajorUint = 0;
constexpr uint8_t kMajorNegint = 1;
constexpr uint8_t kMajorBytes = 2;
constexpr uint8_t kMajorText = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorSimple = 7;

constexpr uint8_t kFalse = 20;
constexpr uint8_t kTrue = 21;

void put_be(std::vector<uint8_t>& out, uint64_t value, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

} // namespace

void put_head(std::vector<uint8_t>& out, uint8_t major, uint64_t argument)
{
    const uint8_t tag = static_cast<uint8_t>(major << 5);
    // Shortest form. RFC 8949 §4.2.1 makes this the deterministic encoding, and two
    // encoders that pick differently produce payloads that differ for no reason.
    if (argument < 24) {
        out.push_back(static_cast<uint8_t>(tag | argument));
    } else if (argument <= 0xFF) {
        out.push_back(static_cast<uint8_t>(tag | 24));
        put_be(out, argument, 1);
    } else if (argument <= 0xFFFF) {
        out.push_back(static_cast<uint8_t>(tag | 25));
        put_be(out, argument, 2);
    } else if (argument <= 0xFFFFFFFF) {
        out.push_back(static_cast<uint8_t>(tag | 26));
        put_be(out, argument, 4);
    } else {
        out.push_back(static_cast<uint8_t>(tag | 27));
        put_be(out, argument, 8);
    }
}

void put_uint(std::vector<uint8_t>& out, uint64_t value)
{
    put_head(out, kMajorUint, value);
}

void put_int(std::vector<uint8_t>& out, int64_t value)
{
    if (value >= 0) {
        put_head(out, kMajorUint, static_cast<uint64_t>(value));
        return;
    }
    // CBOR stores -1-n, so the most negative int64 still fits without overflowing on the
    // way in: negating it first would not.
    const uint64_t n = static_cast<uint64_t>(-(value + 1));
    put_head(out, kMajorNegint, n);
}

void put_bytes(std::vector<uint8_t>& out, std::span<const uint8_t> bytes)
{
    put_head(out, kMajorBytes, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_text(std::vector<uint8_t>& out, std::string_view text)
{
    put_head(out, kMajorText, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

void put_bool(std::vector<uint8_t>& out, bool value)
{
    out.push_back(static_cast<uint8_t>((kMajorSimple << 5) | (value ? kTrue : kFalse)));
}

void put_array(std::vector<uint8_t>& out, uint64_t count)
{
    put_head(out, kMajorArray, count);
}

void put_map(std::vector<uint8_t>& out, uint64_t pairs)
{
    put_head(out, kMajorMap, pairs);
}

bool Reader::head(uint8_t& major, uint64_t& argument)
{
    if (m_in.empty()) {
        return false;
    }
    const uint8_t initial = m_in[0];
    major = static_cast<uint8_t>(initial >> 5);
    const uint8_t extra = static_cast<uint8_t>(initial & 0x1F);

    int width = 0;
    switch (extra) {
    case 24: width = 1; break;
    case 25: width = 2; break;
    case 26: width = 4; break;
    case 27: width = 8; break;
    default:
        if (extra >= 28) {
            // 28..30 are reserved and 31 is an indefinite length, which this subset does
            // not write and will not read: accepting it would mean accepting a stream we
            // cannot bound.
            return false;
        }
        argument = extra;
        m_in = m_in.subspan(1);
        return true;
    }

    if (m_in.size() < static_cast<std::size_t>(1 + width)) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value = (value << 8) | m_in[static_cast<std::size_t>(1 + i)];
    }
    argument = value;
    m_in = m_in.subspan(static_cast<std::size_t>(1 + width));
    return true;
}

bool Reader::uint(uint64_t& value)
{
    const auto saved = m_in;
    uint8_t major = 0;
    uint64_t argument = 0;
    if (!head(major, argument) || major != kMajorUint) {
        m_in = saved;
        return false;
    }
    value = argument;
    return true;
}

bool Reader::integer(int64_t& value)
{
    const auto saved = m_in;
    uint8_t major = 0;
    uint64_t argument = 0;
    if (!head(major, argument)) {
        m_in = saved;
        return false;
    }
    if (major == kMajorUint) {
        value = static_cast<int64_t>(argument);
        return true;
    }
    if (major == kMajorNegint) {
        value = -1 - static_cast<int64_t>(argument);
        return true;
    }
    m_in = saved;
    return false;
}

bool Reader::text(std::string& value)
{
    const auto saved = m_in;
    uint8_t major = 0;
    uint64_t argument = 0;
    if (!head(major, argument) || major != kMajorText || m_in.size() < argument) {
        m_in = saved;
        return false;
    }
    value.assign(reinterpret_cast<const char*>(m_in.data()),
        static_cast<std::size_t>(argument));
    m_in = m_in.subspan(static_cast<std::size_t>(argument));
    return true;
}

bool Reader::boolean(bool& value)
{
    if (m_in.empty()) {
        return false;
    }
    const uint8_t initial = m_in[0];
    if (initial != ((kMajorSimple << 5) | kFalse)
        && initial != ((kMajorSimple << 5) | kTrue)) {
        return false;
    }
    value = (initial & 0x1F) == kTrue;
    m_in = m_in.subspan(1);
    return true;
}

bool Reader::array(uint64_t& count)
{
    const auto saved = m_in;
    uint8_t major = 0;
    uint64_t argument = 0;
    if (!head(major, argument) || major != kMajorArray) {
        m_in = saved;
        return false;
    }
    count = argument;
    return true;
}

bool Reader::map(uint64_t& pairs)
{
    const auto saved = m_in;
    uint8_t major = 0;
    uint64_t argument = 0;
    if (!head(major, argument) || major != kMajorMap) {
        m_in = saved;
        return false;
    }
    pairs = argument;
    return true;
}

bool Reader::skip()
{
    uint8_t major = 0;
    uint64_t argument = 0;
    if (m_in.empty()) {
        return false;
    }
    if (major = static_cast<uint8_t>(m_in[0] >> 5); major == kMajorSimple) {
        // Only the two booleans are written, and both are one byte.
        return !m_in.empty() && (m_in = m_in.subspan(1), true);
    }
    if (!head(major, argument)) {
        return false;
    }
    switch (major) {
    case kMajorUint:
    case kMajorNegint:
        return true;
    case kMajorBytes:
    case kMajorText:
        if (m_in.size() < argument) {
            return false;
        }
        m_in = m_in.subspan(static_cast<std::size_t>(argument));
        return true;
    case kMajorArray:
        for (uint64_t i = 0; i < argument; ++i) {
            if (!skip()) {
                return false;
            }
        }
        return true;
    case kMajorMap:
        for (uint64_t i = 0; i < argument * 2; ++i) {
            if (!skip()) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

} // namespace sve::cbor
