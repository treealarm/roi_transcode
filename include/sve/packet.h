#pragma once

// One encoded access unit and the bytes behind it -- TZ v3.4 §8.6.
//
// The payload is shared and immutable, so copying a packet copies a refcount rather than a
// frame's worth of bytes, and no copy can be modified out from under another.
//
// This replaces a borrowed span whose contract was "valid until the next call on the same
// session". That contract is correct and unenforceable in equal measure: it reads fine at
// the call site, and the one caller who queues a packet instead of writing it immediately
// gets memory that was reused three frames ago. The cost of closing it is one allocation
// and one copy per packet -- at archive bitrates, under a megabyte a second.

#include "sve/timestamp.h"
#include "sve/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace sve {

/// Encoded bytes, shared and immutable. Empty is a valid state: a default-constructed
/// buffer owns nothing and reports size zero rather than dereferencing anything.
class PacketBuffer {
public:
    PacketBuffer() = default;

    explicit PacketBuffer(std::vector<uint8_t> data)
        : m_bytes(std::make_shared<const std::vector<uint8_t>>(std::move(data)))
    {
    }

    /// Takes a copy of bytes the caller owns. Named rather than a constructor, because a
    /// span and a vector overload of the same name is exactly where a copy happens by
    /// accident.
    static PacketBuffer Copy(std::span<const uint8_t> bytes)
    {
        return PacketBuffer(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    const uint8_t* data() const noexcept { return m_bytes ? m_bytes->data() : nullptr; }
    std::size_t size() const noexcept { return m_bytes ? m_bytes->size() : 0; }
    bool empty() const noexcept { return size() == 0; }

    std::span<const uint8_t> bytes() const noexcept
    {
        return {data(), size()};
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> m_bytes;
};

struct EncodedPacket {
    PacketBuffer payload;

    int64_t pts = kNoTimestamp;
    int64_t dts = kNoTimestamp;
    int64_t duration = 0;

    bool key_frame = false;

    /// What this packet is. Never Codec::SameAsSource -- that one is resolved to a real
    /// codec before any backend is asked for anything, so no packet can carry it.
    Codec codec = Codec::H265;

    /// Units of pts, dts and duration. One base for the three of them: they are counted
    /// on the same clock, and three copies of a rational is three chances to disagree.
    Rational time_base{1, 1};
};

} // namespace sve
