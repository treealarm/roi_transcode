// No silent degradation, in the four places that implement it.
//
// Each guard answers the same question for one degradation: the caller asked for something
// this encoder cannot do, so either say so and refuse (EngineFallback::Strict), or do the
// lesser thing and say that too (Permissive). What must not happen is the third option,
// which is to do the lesser thing quietly, and the only way to tell that apart from the
// second is to look at whether anything was logged.
//
// Two properties per guard, and the second is the one that a change would break silently:
//
//   * Strict refuses with Unsupported, carrying the same text the log would have had;
//   * Permissive returns Ok, logs once, and does not log again -- these are called per
//     frame, so a guard that forgot its `warned` flag would bury the encode in its own
//     warnings at thirty lines a second.

#include "coding_id.h"
#include "live_tuning_warnings.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ta_roienc;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

/// A logger that keeps what it was told, so "did it say anything" is a question with an
/// answer rather than something to read off a terminal.
struct Collector {
    std::vector<std::string> lines;
    Journal logger()
    {
        return Journal([this](Verbosity, std::string_view text) {
            lines.emplace_back(text);
        });
    }
};

EngineAbilities without_sei()
{
    EngineAbilities caps;
    caps.sei = false;
    caps.qp_offsets = false;
    caps.max_roi_regions = 4;
    return caps;
}

void strict_refuses_and_permissive_reports_once()
{
    const EngineAbilities caps = without_sei();

    // Bitstream metadata asked of an encoder that cannot carry it.
    {
        Collector strict_log;
        bool warned = false;
        const Verdict s = guard_sei_unsupported(true, caps, EngineFallback::Strict,
            strict_log.logger(), warned);
        check(s.code() == VerdictCode::Unsupported, "strict SEI guard refuses");
        check(!s.message().empty() && strict_log.lines.size() == 1,
            "strict SEI guard says why, once");

        Collector permissive_log;
        bool warned_p = false;
        check(guard_sei_unsupported(true, caps, EngineFallback::Permissive,
                  permissive_log.logger(), warned_p).ok(),
            "permissive SEI guard continues");
        check(permissive_log.lines.size() == 1, "permissive SEI guard warns");
        guard_sei_unsupported(true, caps, EngineFallback::Permissive,
            permissive_log.logger(), warned_p);
        check(permissive_log.lines.size() == 1,
            "and does not warn again -- this runs per frame");
    }

    // Regions cut down to what the backend accepts.
    {
        Collector strict_log;
        bool warned = false;
        const Verdict s = guard_roi_regions_dropped(3, caps, Coding::H264,
            EngineFallback::Strict, strict_log.logger(), warned);
        check(s.code() == VerdictCode::Unsupported, "strict region guard refuses");
        check(s.message().find("4") != std::string::npos,
            "and names the limit the backend gave");

        Collector permissive_log;
        bool warned_p = false;
        check(guard_roi_regions_dropped(3, caps, Coding::H265, EngineFallback::Permissive,
                  permissive_log.logger(), warned_p).ok(),
            "permissive region guard continues");
        check(permissive_log.lines.size() == 1, "permissive region guard warns");
        guard_roi_regions_dropped(3, caps, Coding::H265, EngineFallback::Permissive,
            permissive_log.logger(), warned_p);
        check(permissive_log.lines.size() == 1, "and warns only once");
    }

    // A per-block map handed to a backend that takes rectangles.
    {
        Collector strict_log;
        bool warned = false;
        const Verdict s = guard_qp_map_quantized(true, caps, EngineFallback::Strict,
            strict_log.logger(), warned);
        check(s.code() == VerdictCode::Unsupported, "strict qp-map guard refuses");

        Collector permissive_log;
        bool warned_p = false;
        check(guard_qp_map_quantized(true, caps, EngineFallback::Permissive,
                  permissive_log.logger(), warned_p).ok(),
            "permissive qp-map guard continues");
        check(permissive_log.lines.size() == 1, "permissive qp-map guard warns");
    }
}

/// Nothing to report is reported as nothing: a guard that fires when the capability is
/// present would refuse encodes that are perfectly fine, which is the opposite failure and
/// just as silent from the caller's side.
void a_guard_says_nothing_when_there_is_nothing_to_say()
{
    EngineAbilities caps;
    caps.sei = true;
    caps.qp_offsets = true;
    caps.max_roi_regions = 0;

    Collector log;
    bool warned = false;
    check(guard_sei_unsupported(true, caps, EngineFallback::Strict, log.logger(), warned).ok(),
        "an encoder that carries SEI is not refused");
    check(guard_qp_map_quantized(true, caps, EngineFallback::Strict, log.logger(), warned).ok(),
        "an encoder with a block map is not refused");
    check(guard_roi_regions_dropped(0, caps, Coding::H264, EngineFallback::Strict,
              log.logger(), warned).ok(),
        "nothing dropped is nothing to report");
    check(log.lines.empty(), "and none of them said anything");
}

/// The codec, in both vocabularies. Small, documented, and never checked: "anything that is
/// not HEVC becomes H.264" is the SameAsSource rule, and the reverse direction has to agree
/// with it or a file gets a tag its bitstream does not match.
void the_codec_survives_both_directions()
{
    check(codec_from_av_id(AV_CODEC_ID_H264) == Coding::H264, "H.264 comes back as H.264");
    check(codec_from_av_id(AV_CODEC_ID_HEVC) == Coding::H265, "HEVC comes back as HEVC");
    // The documented rule for everything else, spelled with a codec nothing here encodes.
    check(codec_from_av_id(AV_CODEC_ID_VP9) == Coding::H264,
        "anything that is not HEVC becomes H.264");

    check(av_id_from_codec(Coding::H264) == AV_CODEC_ID_H264, "H.264 goes out as H.264");
    check(av_id_from_codec(Coding::H265) == AV_CODEC_ID_HEVC, "HEVC goes out as HEVC");
    check(codec_from_av_id(av_id_from_codec(Coding::H264)) == Coding::H264, "H.264 round trip");
    check(codec_from_av_id(av_id_from_codec(Coding::H265)) == Coding::H265, "HEVC round trip");
}

} // namespace

int main()
{
    strict_refuses_and_permissive_reports_once();
    a_guard_says_nothing_when_there_is_nothing_to_say();
    the_codec_survives_both_directions();

    if (failures > 0) {
        std::fprintf(stderr, "runtime_warnings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("runtime_warnings_test ok");
    return 0;
}
