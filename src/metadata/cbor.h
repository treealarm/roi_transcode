#pragma once

// The slice of CBOR (RFC 8949) this library's metadata needs -- TZ v3.4 §19.2, which makes
// CBOR the one normative payload format for P0.
//
// Written here rather than pulled in. vcpkg has three CBOR ports and none of them is used,
// because the library's dependency set is FFmpeg and nothing else -- that is what lets
// another project embed it with add_subdirectory(), and it is written down in vcpkg.json as
// a deliberate property rather than an accident. A hundred lines of encoder is a smaller
// price than that property.
//
// What that costs is the risk of writing almost-CBOR, so conformance is proven rather than
// asserted: cbor_test checks these encoders byte for byte against the vectors in RFC 8949
// Appendix A, which is the same thing a third-party decoder will expect.
//
// The subset: unsigned and negative integers, byte and text strings, arrays and maps, plus
// true/false. Deterministically encoded -- shortest form for every head, definite lengths
// throughout, map keys in the order written. Floats are deliberately absent: a confidence
// travels as an integer scaled by 1000, because a float has more than one valid encoding
// and two encoders that disagree about which produce payloads that differ without meaning
// to.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sve::cbor {

/// Appends one item's head: the major type in the top three bits, then the argument in the
/// shortest form that holds it.
void put_head(std::vector<uint8_t>& out, uint8_t major, uint64_t argument);

void put_uint(std::vector<uint8_t>& out, uint64_t value);
/// Encodes as major type 0 or 1 depending on sign, which is how CBOR spells integers.
void put_int(std::vector<uint8_t>& out, int64_t value);
void put_bytes(std::vector<uint8_t>& out, std::span<const uint8_t> bytes);
void put_text(std::vector<uint8_t>& out, std::string_view text);
void put_bool(std::vector<uint8_t>& out, bool value);
/// Definite-length heads. The count is the number of items, or of pairs for a map.
void put_array(std::vector<uint8_t>& out, uint64_t count);
void put_map(std::vector<uint8_t>& out, uint64_t pairs);

/// Reads back what the above writes. Every function consumes from `in` on success and
/// leaves it untouched on failure, so a caller can stop at the first false.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) : m_in(in) {}

    bool uint(uint64_t& value);
    bool integer(int64_t& value);
    bool text(std::string& value);
    bool boolean(bool& value);
    bool array(uint64_t& count);
    bool map(uint64_t& pairs);

    /// Skips one complete item, whatever it is, including the contents of a container.
    /// This is what lets a reader tolerate a field it does not know.
    bool skip();

    bool done() const { return m_in.empty(); }
    std::span<const uint8_t> rest() const { return m_in; }

private:
    bool head(uint8_t& major, uint64_t& argument);

    std::span<const uint8_t> m_in;
};

} // namespace sve::cbor
