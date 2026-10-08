// CBOR, checked against the standard rather than against itself.
//
// A round-trip test proves only that this code agrees with itself, and the whole point of
// CBOR here is that somebody else's decoder can read what we write. So the encodings below
// are the vectors from RFC 8949 Appendix A, compared byte for byte, and the CRC-32C values
// are the published ones for their inputs.

#include "../../src/metadata/cbor.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace sve;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

std::string hex(const std::vector<uint8_t>& v)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : v) {
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

void expect(const std::vector<uint8_t>& got, const char* want, int line)
{
    if (hex(got) != want) {
        std::fprintf(stderr, "fail %s:%d: encoded %s, RFC 8949 says %s\n", __FILE__, line,
            hex(got).c_str(), want);
        std::exit(1);
    }
}

#define EXPECT(bytes, want) expect(bytes, want, __LINE__)

void unsigned_integers_match_the_rfc()
{
    std::vector<uint8_t> out;
    cbor::put_uint(out, 0);            EXPECT(out, "00"); out.clear();
    cbor::put_uint(out, 1);            EXPECT(out, "01"); out.clear();
    cbor::put_uint(out, 10);           EXPECT(out, "0a"); out.clear();
    cbor::put_uint(out, 23);           EXPECT(out, "17"); out.clear();
    cbor::put_uint(out, 24);           EXPECT(out, "1818"); out.clear();
    cbor::put_uint(out, 25);           EXPECT(out, "1819"); out.clear();
    cbor::put_uint(out, 100);          EXPECT(out, "1864"); out.clear();
    cbor::put_uint(out, 1000);         EXPECT(out, "1903e8"); out.clear();
    cbor::put_uint(out, 1000000);      EXPECT(out, "1a000f4240"); out.clear();
    cbor::put_uint(out, 1000000000000ull);
    EXPECT(out, "1b000000e8d4a51000"); out.clear();
    cbor::put_uint(out, 18446744073709551615ull);
    EXPECT(out, "1bffffffffffffffff"); out.clear();
}

void negative_integers_match_the_rfc()
{
    std::vector<uint8_t> out;
    cbor::put_int(out, -1);            EXPECT(out, "20"); out.clear();
    cbor::put_int(out, -10);           EXPECT(out, "29"); out.clear();
    cbor::put_int(out, -100);          EXPECT(out, "3863"); out.clear();
    cbor::put_int(out, -1000);         EXPECT(out, "3903e7"); out.clear();
    // The most negative int64. Negating it before encoding would overflow; CBOR stores
    // -1-n, so the encoder must subtract first.
    cbor::put_int(out, INT64_MIN);     EXPECT(out, "3b7fffffffffffffff"); out.clear();
}

void strings_arrays_and_maps_match_the_rfc()
{
    std::vector<uint8_t> out;
    cbor::put_text(out, "");           EXPECT(out, "60"); out.clear();
    cbor::put_text(out, "a");          EXPECT(out, "6161"); out.clear();
    cbor::put_text(out, "IETF");       EXPECT(out, "6449455446"); out.clear();
    cbor::put_text(out, "\"\\");       EXPECT(out, "62225c"); out.clear();

    const uint8_t raw[] = {0x01, 0x02, 0x03, 0x04};
    cbor::put_bytes(out, raw);         EXPECT(out, "4401020304"); out.clear();

    cbor::put_array(out, 0);           EXPECT(out, "80"); out.clear();
    cbor::put_array(out, 3);
    cbor::put_uint(out, 1);
    cbor::put_uint(out, 2);
    cbor::put_uint(out, 3);            EXPECT(out, "83010203"); out.clear();

    cbor::put_map(out, 0);             EXPECT(out, "a0"); out.clear();
    cbor::put_map(out, 2);
    cbor::put_uint(out, 1);
    cbor::put_uint(out, 2);
    cbor::put_uint(out, 3);
    cbor::put_uint(out, 4);            EXPECT(out, "a201020304"); out.clear();

    cbor::put_bool(out, false);        EXPECT(out, "f4"); out.clear();
    cbor::put_bool(out, true);         EXPECT(out, "f5"); out.clear();

    // A 25-element array, which is where the head stops fitting in the initial byte.
    cbor::put_array(out, 25);
    for (uint64_t i = 1; i <= 25; ++i) {
        cbor::put_uint(out, i);
    }
    EXPECT(out, "98190102030405060708090a0b0c0d0e0f101112131415161718181819");
}

void what_is_written_reads_back()
{
    std::vector<uint8_t> out;
    cbor::put_array(out, 5);
    cbor::put_uint(out, 42);
    cbor::put_int(out, -7);
    cbor::put_text(out, "plate");
    cbor::put_bool(out, true);
    cbor::put_array(out, 2);
    cbor::put_uint(out, 1);
    cbor::put_uint(out, 2);

    cbor::Reader r(out);
    uint64_t count = 0;
    CHECK(r.array(count) && count == 5);
    uint64_t u = 0;
    CHECK(r.uint(u) && u == 42);
    int64_t i = 0;
    CHECK(r.integer(i) && i == -7);
    std::string text;
    CHECK(r.text(text) && text == "plate");
    bool flag = false;
    CHECK(r.boolean(flag) && flag);
    uint64_t inner = 0;
    CHECK(r.array(inner) && inner == 2);
    CHECK(r.uint(u) && u == 1);
    CHECK(r.uint(u) && u == 2);
    CHECK(r.done());
}

void a_reader_leaves_the_stream_alone_when_the_type_is_wrong()
{
    std::vector<uint8_t> out;
    cbor::put_text(out, "not a number");
    cbor::Reader r(out);
    uint64_t u = 0;
    CHECK(!r.uint(u));
    // The failed read consumed nothing, so the right one still works.
    std::string text;
    CHECK(r.text(text) && text == "not a number");
}

void skip_steps_over_a_whole_item()
{
    std::vector<uint8_t> out;
    cbor::put_array(out, 2);
    cbor::put_uint(out, 1);
    cbor::put_uint(out, 2);
    cbor::put_text(out, "after");

    cbor::Reader r(out);
    CHECK(r.skip());          // the array and both of its members
    std::string text;
    CHECK(r.text(text) && text == "after");
    CHECK(r.done());
}

void indefinite_lengths_are_refused()
{
    // 0x9f is an indefinite-length array. This subset does not write one, and reading it
    // would mean accepting a stream whose size cannot be bounded in advance.
    const std::vector<uint8_t> stream{0x9F, 0x01, 0xFF};
    cbor::Reader r(stream);
    uint64_t count = 0;
    CHECK(!r.array(count));
}

void a_truncated_stream_does_not_read_past_its_end()
{
    // Claims eight bytes of argument and provides three.
    const std::vector<uint8_t> stream{0x1B, 0x00, 0x00, 0x00};
    cbor::Reader r(stream);
    uint64_t u = 0;
    CHECK(!r.uint(u));

    // Claims a five-byte string and provides two.
    const std::vector<uint8_t> short_text{0x65, 'a', 'b'};
    cbor::Reader r2(short_text);
    std::string text;
    CHECK(!r2.text(text));
}


/// The two middle argument widths: sixteen and thirty-two bits.
///
/// CBOR spends one byte on an argument below 24, then jumps to 1, 2, 4 or 8 extra bytes,
/// and the middle is where a real payload lives: a sidecar record with 300 objects writes a
/// 16-bit array header, and a PTS in microseconds writes a 32-bit one.
void the_middle_argument_widths_match_the_rfc()
{
    std::vector<uint8_t> out;
    // 16-bit: the first value that needs it, and the last it can hold.
    cbor::put_uint(out, 256);          EXPECT(out, "190100"); out.clear();
    cbor::put_uint(out, 65535);        EXPECT(out, "19ffff"); out.clear();
    // 32-bit: the same two boundaries one width up.
    cbor::put_uint(out, 65536);        EXPECT(out, "1a00010000"); out.clear();
    cbor::put_uint(out, 4294967295ull); EXPECT(out, "1affffffff"); out.clear();
    // And the first value that does not fit in 32 bits takes the 64-bit form.
    cbor::put_uint(out, 4294967296ull); EXPECT(out, "1b0000000100000000"); out.clear();

    // Array and map headers use the same widths -- that is the point of put_head being one
    // function -- so a big collection is the same encoding with a different major type.
    cbor::put_array(out, 300);         EXPECT(out, "99012c"); out.clear();
    cbor::put_map(out, 70000);         EXPECT(out, "ba00011170"); out.clear();
}

/// put_int with a non-negative value: the branch that writes an unsigned head from the
/// signed entry point. Every existing call passed it something negative, so the positive
/// half of the function had never run.
void put_int_writes_both_signs()
{
    std::vector<uint8_t> out;
    cbor::put_int(out, 0);             EXPECT(out, "00"); out.clear();
    cbor::put_int(out, 42);            EXPECT(out, "182a"); out.clear();
    cbor::put_int(out, -42);           EXPECT(out, "3829"); out.clear();
    // The extremes, where the sign matters most: CBOR encodes a negative as -1-n, so the
    // most negative int64 is the largest unsigned argument.
    cbor::put_int(out, 9223372036854775807ll);
    EXPECT(out, "1b7fffffffffffffff"); out.clear();
    cbor::put_int(out, -9223372036854775807ll - 1);
    EXPECT(out, "3b7fffffffffffffff"); out.clear();
}

/// Reading maps, and reading signed integers of both signs. Reader::map had no test at all.
void a_reader_reads_maps_and_signed_integers()
{
    std::vector<uint8_t> out;
    cbor::put_map(out, 2);
    cbor::put_text(out, "pts");
    cbor::put_int(out, 1234);          // positive through the signed reader
    cbor::put_text(out, "delta");
    cbor::put_int(out, -9);

    cbor::Reader r(out);
    uint64_t pairs = 0;
    CHECK(r.map(pairs) && pairs == 2);
    std::string key;
    int64_t value = 0;
    CHECK(r.text(key) && key == "pts");
    CHECK(r.integer(value) && value == 1234);
    CHECK(r.text(key) && key == "delta");
    CHECK(r.integer(value) && value == -9);
    CHECK(r.done());
}

/// A map header asked of something that is not a map leaves the stream where it was, the
/// same contract the other readers keep. Without this the failure path in Reader::map --
/// the one that restores the position -- is never taken.
void a_reader_refuses_a_map_that_is_not_there()
{
    std::vector<uint8_t> out;
    cbor::put_array(out, 3);
    cbor::Reader r(out);
    uint64_t pairs = 99;
    CHECK(!r.map(pairs));
    CHECK(pairs == 99);
    // The array is still readable afterwards, which is what "leaves the stream alone" means.
    uint64_t count = 0;
    CHECK(r.array(count) && count == 3);

    // And the same for the signed reader against text.
    std::vector<uint8_t> other;
    cbor::put_text(other, "nine");
    cbor::Reader r2(other);
    int64_t value = 5;
    CHECK(!r2.integer(value));
    CHECK(value == 5);
    std::string text;
    CHECK(r2.text(text) && text == "nine");
}

} // namespace

/// A container that contains a container, on and on: one stack frame per byte until the
/// process dies. These bytes come off a bitstream through decode_sei_payload, so this is
/// reachable from a file rather than from a bug in our own writer.
void nesting_deeper_than_the_format_allows_is_refused_not_followed()
{
    // 0x81 is "an array of one item". Repeated, each byte opens another level.
    std::vector<uint8_t> deep(100000, uint8_t{0x81});
    deep.push_back(0x00); // one integer at the bottom, so only the depth is wrong
    sve::cbor::Reader reader(deep);
    CHECK(!reader.skip());

    // And the legitimate depth still passes: the SEI payload nests three levels, the limit
    // is 32.
    std::vector<uint8_t> shallow(3, uint8_t{0x81});
    shallow.push_back(0x00);
    sve::cbor::Reader ok(shallow);
    CHECK(ok.skip());
    CHECK(ok.done());
}

/// A map whose claimed pair count doubles to zero. Read as `argument * 2` items, 2^63 pairs
/// became no items at all and a truncated map was accepted as a complete one.
void a_pair_count_that_overflows_is_not_read_as_empty()
{
    // 0xBB + eight bytes: a map with 2^63 pairs, and nothing after it.
    const std::vector<uint8_t> absurd{0xBB, 0x80, 0, 0, 0, 0, 0, 0, 0};
    sve::cbor::Reader reader(absurd);
    CHECK(!reader.skip());
}

int main()
{
    nesting_deeper_than_the_format_allows_is_refused_not_followed();
    a_pair_count_that_overflows_is_not_read_as_empty();
    the_middle_argument_widths_match_the_rfc();
    put_int_writes_both_signs();
    a_reader_reads_maps_and_signed_integers();
    a_reader_refuses_a_map_that_is_not_there();
    unsigned_integers_match_the_rfc();
    negative_integers_match_the_rfc();
    strings_arrays_and_maps_match_the_rfc();
    what_is_written_reads_back();
    a_reader_leaves_the_stream_alone_when_the_type_is_wrong();
    skip_steps_over_a_whole_item();
    indefinite_lengths_are_refused();
    a_truncated_stream_does_not_read_past_its_end();
    std::printf("ok\n");
    return 0;
}
