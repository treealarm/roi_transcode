// Every enumerator has a name of its own -- and the library's own function is what says so.
//
// The public API turns enums into text in a dozen places: backend_name, status_code_name,
// pixel_format_name, preset_name, codec_profile_name, log_level_name, encoder_state_name,
// object_priority_name, memory_type_name, fallback_policy_name. Each is a switch with a
// fallback string at the end, and a fallback is exactly what an enumerator added later
// falls into: the build stays green, the report says "?", and whoever reads it learns
// nothing.
//
// So this asks three things of every enumerator, from the shared table in enum_table.h:
//
//   * the name is not empty,
//   * it is not the fallback the function returns for a value it does not know,
//   * and no two enumerators of one enum share it -- a copied case label is the other way
//     these functions go wrong, and it produces a report that is worse than "?" because it
//     looks right.

#include "enum_table.h"

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/frame_buffer.h"
#include "sve/logging.h"
#include "sve/memory.h"
#include "sve/object_priority.h"
#include "sve/policy.h"
#include "sve/status.h"
#include "sve/types.h"

#include <cstdio>
#include <map>
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

/// `fallback` is what the function under test returns for a value that is no enumerator --
/// "?" for most of them, and the name of a real enumerator for the two that answer with one.
/// Passed in rather than guessed, because getting it wrong would make this test vacuous.
template <class E, class Fn>
void every_enumerator_is_named(const char* enum_name, Fn name_of, const char* fallback)
{
    std::map<std::string, int> seen;
    sve_enum_table::for_each<E>([&](E value, const char* spelling) {
        const std::string name = name_of(value);
        const std::string where = std::string(enum_name) + "::" + spelling;
        check(!name.empty(), where + " has an empty name");
        check(name != fallback,
            where + " falls through to the unknown-value answer (\"" + fallback + "\")");
        const int previous = seen.count(name) ? seen[name] : -1;
        check(previous < 0, where + " shares the name \"" + name + "\" with another value");
        seen[name] = static_cast<int>(value);
    });
    check(!seen.empty(), std::string(enum_name) + ": no enumerators walked at all");
}

} // namespace

int main()
{
    using namespace sve;

    every_enumerator_is_named<Backend>("Backend", backend_name, "?");
    every_enumerator_is_named<StatusCode>("StatusCode", status_code_name, "?");
    every_enumerator_is_named<LogLevel>("LogLevel", log_level_name, "?");
    every_enumerator_is_named<EncoderState>("EncoderState", encoder_state_name, "?");
    every_enumerator_is_named<ObjectPriority>("ObjectPriority", object_priority_name,
        // This one answers with a real enumerator's name for an unknown value, so the
        // fallback cannot be used as the sentinel: Background is a legitimate answer.
        "");
    // PixelFormat::Unknown is itself an enumerator and its name is the fallback, so the
    // sentinel here is emptiness alone -- the same reason as above, from the other side.
    every_enumerator_is_named<PixelFormat>("PixelFormat", pixel_format_name, "");

    // The enums with no entry in enum_table.h because nothing else needs to walk them, but
    // whose name functions are exactly the kind this test exists for.
    for (const Preset p : {Preset::Fastest, Preset::Fast, Preset::Balanced,
             Preset::Slow, Preset::Slowest}) {
        check(std::string(preset_name(p)) != "?", "Preset has an unnamed value");
    }
    for (const CodecProfile p : {CodecProfile::Auto, CodecProfile::H264Baseline,
             CodecProfile::H264Main, CodecProfile::H264High, CodecProfile::H265Main,
             CodecProfile::H265Main10}) {
        check(std::string(codec_profile_name(p)) != "?", "CodecProfile has an unnamed value");
    }
    for (const MemoryType m : {MemoryType::CPU, MemoryType::AVFRAME, MemoryType::VAAPI,
             MemoryType::ONEVPL}) {
        check(std::string(memory_type_name(m)) != "?", "MemoryType has an unnamed value");
    }
    for (const FallbackPolicy f : {FallbackPolicy::Strict, FallbackPolicy::Permissive}) {
        check(std::string(fallback_policy_name(f)) != "?",
            "FallbackPolicy has an unnamed value");
    }

    // The mirror of everything above: a name the library prints has to parse back to the
    // value it came from. Both directions are public API -- the CLI and a config file spell
    // presets and backends by name -- and a parser that quietly answers with its default is
    // the failure that looks like a working configuration. preset_from_name documents
    // exactly that behaviour for unknown text, which is why the round trip is what has to
    // be checked rather than the parse alone.
    for (const Preset p : {Preset::Fastest, Preset::Fast, Preset::Balanced,
             Preset::Slow, Preset::Slowest}) {
        const char* name = preset_name(p);
        check(preset_from_name(name) == p,
            std::string("preset \"") + name + "\" does not parse back to itself");
    }

    // Backends parse into a choice rather than a value: "auto" and "cpu" name an order
    // rather than one backend, so what round-trips is the kind, and the two spellings that
    // name a list are checked for what they are instead.
    sve_enum_table::for_each<Backend>([](Backend value, const char* spelling) {
        const char* name = backend_name(value);
        const BackendChoice choice = backend_choice_from_name(name);
        check(choice.kind == value,
            std::string("backend \"") + name + "\" (" + spelling + ") parses back to "
                + backend_name(choice.kind));
    });
    check(backend_choice_from_name("cpu").selection.preferred.size() > 1,
        "\"cpu\" should name more than one backend");

    if (failures > 0) {
        std::fprintf(stderr, "names_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("names_test ok");
    return 0;
}
