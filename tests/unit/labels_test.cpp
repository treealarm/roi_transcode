// Every enumerator has a name of its own -- and the library's own function is what says so.
//
// The public API turns enums into text in a dozen places: engine_name, verdict_code_name,
// pixel_layout_name, effort_name, coding_profile_name, verbosity_name, lifecycle_name,
// salience_name, memory_kind_name, engine_fallback_name. Each is a switch with a
// fallback string at the end, and a fallback is exactly what an enumerator added later
// falls into: the build stays green, the report says "?", and whoever reads it learns
// nothing.
//
// So this asks three things of every enumerator, from the shared table in enum_names.h:
//
//   * the name is not empty,
//   * it is not the fallback the function returns for a value it does not know,
//   * and no two enumerators of one enum share it -- a copied case label is the other way
//     these functions go wrong, and it produces a report that is worse than "?" because it
//     looks right.

#include "enum_names.h"

#include "ta_roienc/engine_order.h"
#include "ta_roienc/encode.h"
#include "ta_roienc/picture_store.h"
#include "ta_roienc/log.h"
#include "ta_roienc/memory_kind.h"
#include "ta_roienc/salience.h"
#include "ta_roienc/rules.h"
#include "ta_roienc/verdict.h"
#include "ta_roienc/basics.h"

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
    ta_roienc_enum_table::for_each<E>([&](E value, const char* spelling) {
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
    using namespace ta_roienc;

    every_enumerator_is_named<Engine>("Engine", engine_name, "?");
    every_enumerator_is_named<VerdictCode>("VerdictCode", verdict_code_name, "?");
    every_enumerator_is_named<Verbosity>("Verbosity", verbosity_name, "?");
    every_enumerator_is_named<Lifecycle>("Lifecycle", lifecycle_name, "?");
    every_enumerator_is_named<Salience>("Salience", salience_name,
        // This one answers with a real enumerator's name for an unknown value, so the
        // fallback cannot be used as the sentinel: Background is a legitimate answer.
        "");
    // PixelLayout::Unknown is itself an enumerator and its name is the fallback, so the
    // sentinel here is emptiness alone -- the same reason as above, from the other side.
    every_enumerator_is_named<PixelLayout>("PixelLayout", pixel_layout_name, "");

    // The enums with no entry in enum_names.h because nothing else needs to walk them, but
    // whose name functions are exactly the kind this test exists for.
    for (const Effort p : {Effort::Fastest, Effort::Fast, Effort::Balanced,
             Effort::Slow, Effort::Slowest}) {
        check(std::string(effort_name(p)) != "?", "Effort has an unnamed value");
    }
    for (const CodingProfile p : {CodingProfile::Auto, CodingProfile::H264Baseline,
             CodingProfile::H264Main, CodingProfile::H264High, CodingProfile::H265Main,
             CodingProfile::H265Main10}) {
        check(std::string(coding_profile_name(p)) != "?", "CodingProfile has an unnamed value");
    }
    for (const MemoryKind m : {MemoryKind::CPU, MemoryKind::AVFRAME, MemoryKind::VAAPI,
             MemoryKind::ONEVPL}) {
        check(std::string(memory_kind_name(m)) != "?", "MemoryKind has an unnamed value");
    }
    for (const EngineFallback f : {EngineFallback::Strict, EngineFallback::Permissive}) {
        check(std::string(engine_fallback_name(f)) != "?",
            "EngineFallback has an unnamed value");
    }

    // The mirror of everything above: a name the library prints has to parse back to the
    // value it came from. Both directions are public API -- the CLI and a config file spell
    // presets and backends by name -- and a parser that quietly answers with its default is
    // the failure that looks like a working configuration. effort_from_name documents
    // exactly that behaviour for unknown text, which is why the round trip is what has to
    // be checked rather than the parse alone.
    for (const Effort p : {Effort::Fastest, Effort::Fast, Effort::Balanced,
             Effort::Slow, Effort::Slowest}) {
        const char* name = effort_name(p);
        check(effort_from_name(name) == p,
            std::string("preset \"") + name + "\" does not parse back to itself");
    }

    // Backends parse into a choice rather than a value: "auto" and "cpu" name an order
    // rather than one backend, so what round-trips is the kind, and the two spellings that
    // name a list are checked for what they are instead.
    ta_roienc_enum_table::for_each<Engine>([](Engine value, const char* spelling) {
        const char* name = engine_name(value);
        const EngineChoice choice = engine_choice_from_name(name);
        check(choice.kind == value,
            std::string("backend \"") + name + "\" (" + spelling + ") parses back to "
                + engine_name(choice.kind));
    });
    check(engine_choice_from_name("cpu").selection.preferred.size() > 1,
        "\"cpu\" should name more than one backend");

    if (failures > 0) {
        std::fprintf(stderr, "names_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("names_test ok");
    return 0;
}
