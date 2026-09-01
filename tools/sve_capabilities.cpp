// sve-capabilities -- what this build can do on this machine. TZ v4.2 §23.
//
//     sve-capabilities [--json]
//
// Built with SVE_BUILD_TOOLS=ON and linked against the public library, as §23 requires.
// It uses nothing but include/: the registry of §26.4 lists what was linked, and
// IEncoderBackend::GetCapabilities answers for each. If this file stops compiling against
// the public headers, the public headers have lost something a caller needs.
//
// Two departures from §23's example, both deliberate and both because the example cannot
// express what is true here:
//
//   * capabilities are reported per codec, not once per backend. §23 shows a single
//     max_roi_regions, and on Intel that number is four for H.264 and eight for H.265 --
//     measured, not assumed, and recorded in STATUS.md. A flat report would have to pick
//     one and be wrong about the other.
//
//   * "device" is present and empty. §21's backend interface has no device query, so there
//     is nothing to fill it with. Printing the backend's own name there would look like an
//     answer; an empty string says plainly that we do not know yet.

#include "sve/backend_registry.h"
#include "sve/capabilities.h"
#include "sve/encoder_backend.h"
#include "sve/types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// A nominal target, only its non-zero-ness matters -- see CapabilityQuery.
constexpr uint64_t kNominalTarget = 4'000'000;

struct CodecReport {
    const char* name;
    sve::Codec codec;
    bool supported = false;
    /// What the backend answers with no target bitrate asked for.
    sve::EncoderCapabilities caps;
    /// And what it answers with one.
    ///
    /// Reported separately since 2026-09-01, because on the native oneVPL backend the two
    /// differ: it has a per-block QP map under CQP and hardware VBR under a target, and not
    /// both. A report that printed only one of them would be telling a reader that a
    /// feature is there when whether it is there depends on what they ask for.
    sve::EncoderCapabilities caps_with_target;
};

const char* yes_no(bool value) { return value ? "true" : "false"; }

void print_json(const std::vector<std::pair<sve::Backend, std::vector<CodecReport>>>& all)
{
    std::printf("{\n  \"backends\": [\n");
    for (std::size_t b = 0; b < all.size(); ++b) {
        const auto& [kind, codecs] = all[b];
        std::printf("    {\n");
        std::printf("      \"backend\": \"%s\",\n", sve::backend_name(kind));
        std::printf("      \"device\": \"\",\n");

        std::printf("      \"codecs\": [");
        bool first = true;
        for (const CodecReport& c : codecs) {
            if (!c.supported) {
                continue;
            }
            std::printf("%s\"%s\"", first ? "" : ", ", c.name);
            first = false;
        }
        std::printf("],\n");
        std::printf("      \"available\": %s,\n", yes_no(!first));

        std::printf("      \"capabilities\": {\n");
        bool wrote = false;
        for (const CodecReport& c : codecs) {
            if (!c.supported) {
                continue;
            }
            if (wrote) {
                std::printf(",\n");
            }
            const sve::EncoderCapabilities& k = c.caps;
            std::printf("        \"%s\": {\n", c.name);
            std::printf("          \"roi_rectangles\": %s,\n", yes_no(k.roi_rectangles));
            std::printf("          \"qp_map\": %s,\n", yes_no(k.qp_map));
            std::printf("          \"qp_map_block\": [%u, %u],\n",
                k.qp_map_block_width, k.qp_map_block_height);
            std::printf("          \"ctu_qp\": %s,\n", yes_no(k.ctu_qp));
            std::printf("          \"emphasis_map\": %s,\n", yes_no(k.emphasis_map));
            std::printf("          \"runtime_roi_update\": %s,\n", yes_no(k.runtime_roi_update));
            std::printf("          \"sei\": %s,\n", yes_no(k.sei));
            std::printf("          \"native_bitrate\": %s,\n", yes_no(k.native_bitrate));
            std::printf("          \"main10\": %s,\n", yes_no(k.main10));
            std::printf("          \"p010_input\": %s,\n", yes_no(k.p010_input));
            std::printf("          \"max_roi_regions\": %u,\n", k.max_roi_regions);
            // §16 and §2.2 are not always available together; where they are not, this says
            // which of them a target bitrate costs.
            std::printf("          \"with_target_bitrate\": { \"qp_map\": %s, "
                        "\"native_bitrate\": %s }\n",
                yes_no(c.caps_with_target.qp_map),
                yes_no(c.caps_with_target.native_bitrate));
            std::printf("        }");
            wrote = true;
        }
        std::printf("%s      }\n", wrote ? "\n" : "");
        std::printf("    }%s\n", b + 1 < all.size() ? "," : "");
    }
    std::printf("  ]\n}\n");
}

void print_text(const std::vector<std::pair<sve::Backend, std::vector<CodecReport>>>& all)
{
    for (const auto& [kind, codecs] : all) {
        std::printf("%s\n", sve::backend_name(kind));
        bool any = false;
        for (const CodecReport& c : codecs) {
            if (!c.supported) {
                continue;
            }
            any = true;
            const sve::EncoderCapabilities& k = c.caps;
            std::printf("  %-5s regions=%-4u roi_rect=%-5s qp_map=%-5s(%ux%u) sei=%-5s "
                        "native_bitrate=%-5s main10=%s\n",
                c.name, k.max_roi_regions, yes_no(k.roi_rectangles), yes_no(k.qp_map),
                k.qp_map_block_width, k.qp_map_block_height, yes_no(k.sei),
                yes_no(k.native_bitrate), yes_no(k.main10));
            // Only when the answer actually changes, so the common case stays one line.
            if (c.caps_with_target.qp_map != k.qp_map
                || c.caps_with_target.native_bitrate != k.native_bitrate) {
                std::printf("        with a target bitrate: qp_map=%-5s native_bitrate=%s\n",
                    yes_no(c.caps_with_target.qp_map),
                    yes_no(c.caps_with_target.native_bitrate));
            }
        }
        if (!any) {
            // Registered but unable to produce anything here: no device, or no encoder in
            // this build of libavcodec. Said rather than omitted -- a backend missing from
            // the list and a backend that cannot run are different answers.
            std::printf("  (registered, but produces no codec on this machine)\n");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr,
                "Usage: sve-capabilities [--json]\n"
                "\n"
                "What this build of the library can do on this machine (TZ §23): the\n"
                "backends linked into it, the codecs each can produce here, and the ROI\n"
                "controls each offers. Reported per codec, because on Intel the region\n"
                "limit differs between H.264 and H.265.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    std::vector<std::pair<sve::Backend, std::vector<CodecReport>>> all;
    const sve::BackendRegistry& registry = sve::default_backend_registry();
    for (const sve::Backend kind : registry.kinds()) {
        const sve::IEncoderBackend* backend = registry.Find(kind);
        if (backend == nullptr) {
            continue;
        }
        std::vector<CodecReport> codecs{
            {"h264", sve::Codec::H264, false, {}},
            {"h265", sve::Codec::H265, false, {}},
        };
        for (CodecReport& c : codecs) {
            c.caps = backend->GetCapabilities(sve::CapabilityQuery{c.codec, 0});
            c.caps_with_target =
                backend->GetCapabilities(sve::CapabilityQuery{c.codec, kNominalTarget});
            c.supported = c.codec == sve::Codec::H264 ? c.caps.h264 : c.caps.h265;
        }
        all.emplace_back(kind, std::move(codecs));
    }

    if (json) {
        print_json(all);
    } else {
        print_text(all);
    }
    return 0;
}
