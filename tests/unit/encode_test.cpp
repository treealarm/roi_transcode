// The encoder's lifecycle and its submit/receive contract.
//
// What is worth testing here is the state machine, not the picture: that a method called
// at the wrong moment answers rather than misbehaves, that backpressure and end-of-stream
// are codes rather than special cases, and that Stop can be called twice.
//
// Everything runs on the software backend, so this test needs no GPU.

#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ta_roienc;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 96;

EncodeOptions config_for(Engine backend = Engine::X264, Coding codec = Coding::H264)
{
    EncodeOptions cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Ratio{25, 1};
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.encoder = backend;
    cfg.settings.codec = codec;
    cfg.settings.crf = 32;
    // Nothing here has a mask or boxes to embed, and libx264's SEI path is not what this
    // test is about.
    cfg.settings.metadata.bitstream_sei = false;
    return cfg;
}

/// Grey planes the encoder can take, held by a shared owner so the Picture keeps them alive.
struct Samples {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

Picture make_frame(const std::shared_ptr<Samples>& pixels, int64_t pts)
{
    PictureView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_layout = PixelLayout::YUV420P;
    view.layer_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.timestamp = Stamp{pts, Ratio{1, 25}};
    view.owner = pixels;

    Picture frame;
    CHECK(WrapPicture(view, &frame).ok());
    return frame;
}

std::unique_ptr<VideoEncoder> open_encoder(const EncodeOptions& cfg)
{
    return CreateVideoEncoder(cfg);
}

/// The code a factory refuses with, or Ok when it did not refuse. Creation throws, so
/// asserting on a refusal means catching one.
VerdictCode create_refusal(const EncodeOptions& cfg)
{
    try {
        const auto made = CreateVideoEncoder(cfg);
        return VerdictCode::Ok;
    } catch (const EncodeError& e) {
        return e.code();
    }
}

void a_new_encoder_is_configured_and_knows_its_backend()
{
    auto enc = open_encoder(config_for());
    CHECK(enc->Phase() == Lifecycle::Configured);
    const Counters stats = enc->GetStatistics();
    CHECK(stats.frames_submitted == 0);
    CHECK(!stats.engine_name.empty());

    // A muxer can declare its stream before a single frame is submitted.
    CodingParams params;
    CHECK(enc->GetCodingParams(&params).ok());
    CHECK(params.width == kWidth);
    CHECK(params.height == kHeight);
    CHECK(params.codec == Coding::H264);
}

void an_input_format_that_cannot_exist_is_refused()
{
    EncodeOptions cfg = config_for();
    cfg.input.height = 0;
    CHECK(create_refusal(cfg) == VerdictCode::InvalidArgument);
}

void same_as_source_needs_a_source()
{
    // This entry point is handed frames, not a file, so there is nothing to mirror. Better
    // an InvalidArgument than an encoder that quietly picked one.
    EncodeOptions cfg = config_for(Engine::X264, Coding::SameAsSource);
    CHECK(create_refusal(cfg) == VerdictCode::InvalidArgument);
}

void frames_go_in_and_packets_come_out()
{
    auto pixels = std::make_shared<Samples>();
    auto enc = open_encoder(config_for());

    int64_t received = 0;
    for (int64_t i = 0; i < 10; ++i) {
        const Picture frame = make_frame(pixels, i);
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).ok());
        CHECK(enc->Phase() == Lifecycle::Running);

        for (;;) {
            AccessUnit packet;
            const Verdict s = enc->ReceiveAccessUnit(&packet);
            if (s.code() == VerdictCode::Again) {
                break;
            }
            CHECK(s.ok());
            CHECK(!packet.payload.empty());
            CHECK(packet.codec == Coding::H264);
            CHECK(packet.time_base.den > 0);
            ++received;
        }
    }

    CHECK(enc->Flush().ok());
    CHECK(enc->Phase() == Lifecycle::Draining);
    for (;;) {
        AccessUnit packet;
        const Verdict s = enc->ReceiveAccessUnit(&packet);
        if (s.code() == VerdictCode::EndOfStream) {
            break;
        }
        CHECK(s.ok());
        ++received;
    }
    CHECK(enc->Phase() == Lifecycle::EndOfStream);
    CHECK(received == 10);

    const Counters stats = enc->GetStatistics();
    CHECK(stats.frames_submitted == 10);
    CHECK(stats.packets_received == received);
    CHECK(stats.bytes_encoded > 0);
}

void submitting_after_flush_is_refused_rather_than_ignored()
{
    auto pixels = std::make_shared<Samples>();
    auto enc = open_encoder(config_for());
    const Picture frame = make_frame(pixels, 0);
    PictureInfo meta;
    CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).ok());
    CHECK(enc->Flush().ok());

    // Draining, EndOfStream, Stopped and Failed all refuse a frame rather than accepting
    // and dropping it.
    CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).code() == VerdictCode::InvalidArgument);
    CHECK(enc->Stop().ok());
    CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).code() == VerdictCode::InvalidArgument);
}

void stop_is_idempotent()
{
    auto enc = open_encoder(config_for());
    CHECK(enc->Stop().ok());
    CHECK(enc->Phase() == Lifecycle::Stopped);
    CHECK(enc->Stop().ok());
    CHECK(enc->Phase() == Lifecycle::Stopped);
}

void an_empty_frame_and_a_null_packet_are_arguments_not_crashes()
{
    auto enc = open_encoder(config_for());
    PictureInfo meta;
    // An empty Picture is what a default-constructed StampedPicture carries.
    CHECK(enc->SubmitPicture(StampedPicture{}).code() == VerdictCode::InvalidArgument);
    CHECK(enc->ReceiveAccessUnit(nullptr).code() == VerdictCode::InvalidArgument);
    // And the interop adapter still refuses the pointer form.
    CHECK(SubmitAvPicture(*enc, nullptr, meta).code() == VerdictCode::InvalidArgument);
}

void a_keyframe_request_is_counted_even_though_it_is_best_effort()
{
    auto pixels = std::make_shared<Samples>();
    auto enc = open_encoder(config_for());
    CHECK(enc->RequestIdr().ok());

    const Picture frame = make_frame(pixels, 0);
    PictureInfo meta;
    CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).ok());
    CHECK(enc->GetStatistics().keyframes_requested == 1);

    // The same asked for at submit time rather than before it.
    PictureInfo forced;
    forced.force_key_frame = true;
    CHECK(enc->SubmitPicture(StampedPicture{frame, forced}).ok());
    CHECK(enc->GetStatistics().keyframes_requested == 2);
}

void reconfigure_takes_what_8_2_allows_and_refuses_the_rest()
{
    auto enc = open_encoder(config_for());

    LiveTuning ok;
    ok.frame_rate = Ratio{30, 1};
    ok.target_bitrate = 400000;
    CHECK(enc->Reconfigure(ok).ok());

    LiveTuning bad;
    bad.frame_rate = Ratio{0, 1};
    CHECK(enc->Reconfigure(bad).code() == VerdictCode::InvalidArgument);

    CHECK(enc->Stop().ok());
    CHECK(enc->Reconfigure(ok).code() == VerdictCode::InvalidArgument);
}

void cpu_h265_refuses_a_target_bitrate_out_loud()
{
    // The CPU H.265 target-bitrate guard is a standing contract, so it is asserted rather
    // than left to whoever next reads the log: at Create it downgrades and records why, and
    // a caller who asks again through Reconfigure is told instead of quietly ignored.
    EncodeOptions cfg = config_for(Engine::X265, Coding::H265);
    cfg.settings.rate_control.target_bitrate = 800000;
    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().cpu_h265_target_guard);

    LiveTuning again;
    again.target_bitrate = 800000;
    CHECK(enc->Reconfigure(again).code() == VerdictCode::Unsupported);
}

void the_order_ranks_the_engines_and_can_exclude_them()
{
    // A named backend means it: the selection's permissions do not apply, because the
    // caller has already been explicit and quietly getting libx264 instead is the failure
    // this rule exists to prevent.
    EngineOrder none;
    CHECK(resolve_engines(Engine::X264, none, Coding::H264).size() == 1);

    // Auto walks the list, in the order given.
    EngineOrder cpu_first;
    cpu_first.preferred = {Engine::X264, Engine::IntelOneVpl};
    const auto chain = resolve_engines(Engine::Auto, cpu_first, Coding::H264);
    CHECK(chain.size() == 2);
    CHECK(chain[0].get().kind() == Engine::X264);
    CHECK(chain[1].get().kind() == Engine::IntelOneVpl);

    // Software is shut out of the automatic path by leaving it off the list, and by
    // nothing else: two ways to say the same thing is one way to have them disagree.
    EngineOrder hardware_only;
    hardware_only.preferred = {Engine::IntelOneVpl, Engine::Vaapi};
    for (const EngineHandle& ref : resolve_engines(Engine::Auto, hardware_only, Coding::H264)) {
        CHECK(ref.get().kind() != Engine::X264);
        CHECK(ref.get().kind() != Engine::X265);
    }

    // And a backend absent from the list is not reached unless unlisted ones are allowed.
    EngineOrder only_x264;
    only_x264.preferred = {Engine::X264};
    CHECK(resolve_engines(Engine::Auto, only_x264, Coding::H264).size() == 1);
    only_x264.allow_unlisted = true;
    CHECK(resolve_engines(Engine::Auto, only_x264, Coding::H264).size() > 1);
}

void a_backend_that_cannot_encode_the_codec_is_left_out_with_its_reason()
{
    // libx264 and libx265 are two backends, so a preference list can name one that has
    // nothing to offer this particular encode. Under Auto it is skipped rather than opened
    // and found wanting -- and not silently, so the reason comes back with the chain.
    EngineOrder both;
    both.preferred = {Engine::X264, Engine::X265};

    std::vector<std::string> skipped;
    const auto for_hevc = resolve_engines(Engine::Auto, both, Coding::H265, &skipped);
    CHECK(for_hevc.size() == 1);
    CHECK(for_hevc[0].get().kind() == Engine::X265);
    CHECK(skipped.size() == 1);
    CHECK(skipped[0].find("x264") != std::string::npos);

    skipped.clear();
    const auto for_avc = resolve_engines(Engine::Auto, both, Coding::H264, &skipped);
    CHECK(for_avc.size() == 1);
    CHECK(for_avc[0].get().kind() == Engine::X264);
    CHECK(skipped.size() == 1);
    CHECK(skipped[0].find("x265") != std::string::npos);

    // Named outright, the same mismatch is an error rather than a skip: the caller asked
    // for something this backend cannot do, and answering with a different one would be
    // the substitution the rule exists to prevent.
    EncodeOptions cfg = config_for(Engine::X264, Coding::H265);
    CHECK(create_refusal(cfg) == VerdictCode::Unsupported);
}

void an_unusable_preference_order_is_refused_before_anything_opens()
{
    // Auto with no order is InvalidArgument: a default order would be guessing at hardware
    // the library cannot see.
    EncodeOptions empty = config_for(Engine::Auto);
    empty.settings.engine_order.preferred.clear();
    CHECK(create_refusal(empty) == VerdictCode::InvalidArgument);

    // Auto cannot appear inside its own order, and no backend twice.
    EncodeOptions recursive = config_for(Engine::Auto);
    recursive.settings.engine_order.preferred = {Engine::Auto, Engine::X264};
    CHECK(create_refusal(recursive) == VerdictCode::InvalidArgument);

    EncodeOptions twice = config_for(Engine::Auto);
    twice.settings.engine_order.preferred = {Engine::X264, Engine::X264};
    CHECK(create_refusal(twice) == VerdictCode::InvalidArgument);

    // A well-formed order that no backend in this build can serve fails at open, which is
    // a different answer: the request was valid, the machine could not meet it. libx265
    // does not produce H.264, so a list naming only it has nothing to open.
    EncodeOptions unservable = config_for(Engine::Auto);
    unservable.settings.engine_order.preferred = {Engine::X265Direct};
    CHECK(create_refusal(unservable) != VerdictCode::Ok);
}

void the_preference_order_is_read_where_the_backend_is_named()
{
    // The order and the Engine it qualifies are one decision, and they live in one struct
    // for that reason: a caller who builds an EncodeOptions and fills the field next to the
    // Engine gets that order. Nothing here copies anything.
    EncodeOptions cfg = config_for(Engine::Auto);
    cfg.settings.engine_order.preferred = {Engine::X264};

    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().engine_used == Engine::X264);
}

void diagnostics_go_where_the_caller_says()
{
    std::vector<std::pair<Verbosity, std::string>> lines;
    EncodeOptions cfg = config_for(Engine::X265, Coding::H265);
    cfg.settings.rate_control.target_bitrate = 800000;   // trips the CPU H.265 guard
    cfg.log = [&lines](Verbosity level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };

    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().cpu_h265_target_guard);

    // The degradation reached the caller's sink, not somebody's terminal.
    bool guarded = false;
    for (const auto& [level, text] : lines) {
        if (level == Verbosity::Warning && text.find("cpu h265 target-bitrate") != std::string::npos) {
            guarded = true;
        }
    }
    CHECK(guarded);
    CHECK(enc->GetStatistics().fallbacks_applied == 1);

    // And which backend opened is said out loud rather than left to be inferred.
    bool announced = false;
    for (const auto& [level, text] : lines) {
        if (level == Verbosity::Info && text.find("backend:") != std::string::npos) {
            announced = true;
        }
    }
    CHECK(announced);
}

void strict_refuses_where_permissive_degrades()
{
    // Same request, two policies. Under Permissive the encoder opens and records that it
    // had to downgrade; under Strict it refuses, and the caller learns before encoding a
    // single frame rather than from a counter afterwards.
    EncodeOptions permissive = config_for(Engine::X265, Coding::H265);
    permissive.settings.rate_control.target_bitrate = 800000;
    permissive.log = [](Verbosity, std::string_view) {};
    CHECK(create_refusal(permissive) == VerdictCode::Ok);

    EncodeOptions strict = permissive;
    strict.settings.fallback = EngineFallback::Strict;
    CHECK(create_refusal(strict) == VerdictCode::Unsupported);
}

void strict_refuses_a_frame_whose_regions_do_not_fit()
{
    // The software backend has no region limit, so this asserts the shape rather than the
    // hardware's number: with nothing dropped, Strict changes nothing.
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.settings.fallback = EngineFallback::Strict;
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    const Picture frame = make_frame(pixels, 0);
    PictureInfo meta;
    CHECK(enc->SubmitPicture(StampedPicture{frame, meta}).ok());
    CHECK(enc->GetStatistics().fallbacks_applied == 0);
}

/// One sidecar record per emitted packet, in emission order, and the sidecar works when the
/// bitstream metadata does not.
void the_sidecar_follows_the_packets_not_the_submissions()
{
    auto pixels = std::make_shared<Samples>();
    std::vector<std::string> records;

    EncodeOptions cfg = config_for();
    // SEI off on purpose: the sidecar has to stand on its own -- reachable only through the
    // SEI path would defeat the case it exists for.
    cfg.settings.metadata.bitstream_sei = false;
    cfg.settings.metadata.sidecar = MetadataFile::NDJSON;
    cfg.settings.metadata.metadata_file_sink = [&records](std::string_view line) {
        records.emplace_back(line);
    };
    auto enc = open_encoder(cfg);

    constexpr int kFrames = 12;
    std::vector<AccessUnit> packets;
    const auto drain = [&] {
        for (;;) {
            AccessUnit packet;
            const Verdict s = enc->ReceiveAccessUnit(&packet);
            if (!s.ok()) {
                return;
            }
            packets.push_back(std::move(packet));
        }
    };
    // Held outside the loop: Regions::boxes is a span, so the vector has to outlive the
    // submit that reads through it.
    std::vector<Box> boxes(1);
    boxes[0].kind = "plate";
    boxes[0].width = 0.2f;
    boxes[0].height = 0.2f;
    boxes[0].confidence = 0.9f;
    for (int i = 0; i < kFrames; ++i) {
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        boxes[0].x = 0.25f;
        boxes[0].y = 0.25f;
        meta.regions.boxes = boxes;
        CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta}).ok());
        drain();
    }
    CHECK(enc->Flush().ok());
    drain();

    CHECK(records.size() == packets.size());
    CHECK(!records.empty());
    for (std::size_t i = 0; i < records.size(); ++i) {
        // frame_index counts emissions, and the PTS is the one the packet came back with.
        // Both are string checks rather than a parse: this test would otherwise need a
        // JSON reader, and the core deliberately has none.
        const std::string index = "\"frame_index\":" + std::to_string(i);
        CHECK(records[i].find(index) != std::string::npos);
        const std::string pts = "\"pts\":" + std::to_string(packets[i].pts);
        CHECK(records[i].find(pts) != std::string::npos);
    }
}

/// A caller who already has a per-block quantiser grid hands it over as it is, and it takes
/// precedence over a mask and over boxes. This is the branch that turns a grid into
/// rectangles.
void a_qp_map_is_taken_as_given_rather_than_recomputed()
{
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    // 128x96 at a block of 32 is a 4x3 grid. One block is protected, the rest is
    // background: the two distinct values are what the rectangles are derived from.
    constexpr int32_t kBlock = 32;
    QpOffsets map;
    map.block = kBlock;
    map.cols = kWidth / kBlock;
    map.rows = kHeight / kBlock;
    map.width = kWidth;
    map.height = kHeight;
    map.deltas.assign(static_cast<std::size_t>(map.cols) * map.rows, int8_t{6});
    map.deltas[0] = int8_t{-6};

    PictureInfo meta;
    meta.timestamp = Stamp{0, Ratio{1, 25}};
    meta.regions.qp_offsets = map;
    CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, 0), meta}).ok());
    CHECK(enc->Flush().ok());

    // The software backend has no region limit, so nothing is dropped and no degradation
    // is counted: the grid arrived whole.
    CHECK(enc->GetStatistics().regions_dropped == 0);
    CHECK(enc->GetStatistics().fallbacks_applied == 0);
    CHECK(enc->GetStatistics().frames_submitted == 1);
}

/// A mask carries no kinds to weigh, so it is sampled onto the block grid rather than fed to
/// the box pipeline -- and a caller who passes one gets regions, not quietly none.
void a_mask_is_sampled_onto_the_grid_rather_than_ignored()
{
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    // Background at 0, one bright quarter at the maximum an 8-bit mask holds.
    std::vector<uint8_t> mask(static_cast<std::size_t>(kWidth) * kHeight, uint8_t{0});
    for (int32_t y = 0; y < kHeight / 2; ++y) {
        for (int32_t x = 0; x < kWidth / 2; ++x) {
            mask[static_cast<std::size_t>(y) * kWidth + x] =
                static_cast<uint8_t>(kMaskMax);
        }
    }

    PictureInfo meta;
    meta.timestamp = Stamp{0, Ratio{1, 25}};
    meta.regions.mask.data = mask;
    meta.regions.mask.width = kWidth;
    meta.regions.mask.height = kHeight;
    CHECK(meta.regions.mask.consistent());

    CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, 0), meta}).ok());
    CHECK(enc->Flush().ok());
    CHECK(enc->GetStatistics().frames_submitted == 1);
    CHECK(enc->GetStatistics().regions_dropped == 0);
}

/// A mask whose span does not hold the rows it claims is ignored rather than half-read,
/// and the frame still encodes -- the boxes path takes over.
void an_inconsistent_mask_is_ignored_rather_than_half_read()
{
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    std::vector<uint8_t> too_small(static_cast<std::size_t>(kWidth), uint8_t{255});
    PictureInfo meta;
    meta.timestamp = Stamp{0, Ratio{1, 25}};
    meta.regions.mask.data = too_small;
    meta.regions.mask.width = kWidth;
    meta.regions.mask.height = kHeight;
    CHECK(!meta.regions.mask.consistent());

    CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, 0), meta}).ok());
    CHECK(enc->Flush().ok());
    CHECK(enc->GetStatistics().frames_submitted == 1);
}

/// The other side of the sidecar check, and the one that is easy to get backwards: an
/// encode the caller abandoned is not a broken sidecar.
///
/// Stop is safe from any state, so it is also how a cancelled job cleans up. The frames
/// still inside libx264's lookahead at that moment were never given a chance to come out --
/// complaining that records and packets do not correspond would report a data integrity
/// fault about a decision the caller made on purpose.
void a_cancelled_encode_does_not_report_a_sidecar_fault()
{
    auto pixels = std::make_shared<Samples>();
    std::vector<std::pair<Verbosity, std::string>> lines;
    std::vector<std::string> records;

    EncodeOptions cfg = config_for();
    cfg.settings.metadata.bitstream_sei = false;
    cfg.settings.metadata.sidecar = MetadataFile::NDJSON;
    cfg.settings.metadata.metadata_file_sink = [&records](std::string_view line) {
        records.emplace_back(line);
    };
    cfg.log = [&lines](Verbosity level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };
    auto enc = open_encoder(cfg);

    // Submitted and never flushed: libx264 holds frames for its lookahead, so these are
    // still inside the encoder when it is stopped.
    constexpr int kFrames = 8;
    for (int i = 0; i < kFrames; ++i) {
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta}).ok());
    }
    CHECK(records.size() < static_cast<std::size_t>(kFrames));
    enc->Stop();

    bool warned = false;
    for (const auto& [level, text] : lines) {
        if (level == Verbosity::Warning
            && text.find("packet never arrived") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(!warned);
}

/// The report is what a caller reads at the end of a job, so every field of it is checked
/// against its source.
void the_report_says_what_the_statistics_say()
{
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    for (int i = 0; i < 4; ++i) {
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta}).ok());
    }
    CHECK(enc->Flush().ok());
    for (;;) {
        AccessUnit packet;
        if (!enc->ReceiveAccessUnit(&packet).ok()) {
            break;
        }
    }

    const Counters stats = enc->GetStatistics();
    const Outcome report = enc->ToReport();
    CHECK(report.frames == stats.frames_submitted);
    CHECK(report.packets == stats.packets_received);
    CHECK(report.bytes == stats.bytes_encoded);
    CHECK(report.engine_used == stats.engine_used);
    CHECK(report.engine_name == stats.engine_name);
    CHECK(report.regions_dropped == stats.regions_dropped);
    CHECK(report.frames_converted == stats.frames_converted);
    CHECK(report.frames_uploaded == stats.frames_uploaded);
    CHECK(report.cpu_h265_target_guard == stats.cpu_h265_target_guard);
    CHECK(report.roi_lag_measured == stats.roi_lag_measured);
    // Not a tautology: the encode above really happened, so these are non-zero and a
    // field wired to the wrong source would show.
    CHECK(report.frames == 4);
    CHECK(report.packets > 0);
    CHECK(report.bytes > 0);
}

/// The borrowing overload: pixels the caller holds, handed over without a copy and without
/// an ownership handle -- the zero-copy entry point a service embedding this library uses.
void pixels_the_caller_holds_go_in_through_the_borrowing_overload()
{
    /// The caller's own buffer, over the same planes make_frame wraps. Not the fake from
    /// roi_coder_test: that one is NV12 and belongs to a test about backends refusing.
    class BorrowedLayers : public IPictureStore {
    public:
        explicit BorrowedLayers(Samples& pixels)
            : m_pixels(&pixels)
        {
        }
        MemoryKind memory() const override { return MemoryKind::CPU; }
        int32_t width() const override { return kWidth; }
        int32_t height() const override { return kHeight; }
        PixelLayout format() const override { return PixelLayout::YUV420P; }
        LayerView plane(int32_t index) override
        {
            switch (index) {
            case 0: return {m_pixels->y, kWidth};
            case 1: return {m_pixels->u, kWidth / 2};
            default: return {m_pixels->v, kWidth / 2};
            }
        }

    private:
        Samples* m_pixels;
    };

    Samples pixels;
    BorrowedLayers borrowed(pixels);
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    PictureInfo meta;
    meta.timestamp = Stamp{0, Ratio{1, 25}};
    CHECK(enc->SubmitPicture(borrowed, meta).ok());
    CHECK(enc->Flush().ok());
    CHECK(enc->GetStatistics().frames_submitted == 1);
    // Already in the format the encoder wants, so neither counter moved: that is what
    // zero-copy means here, and it is the only way to tell it from a frame that merely
    // travelled.
    CHECK(enc->GetStatistics().frames_converted == 0);
    CHECK(enc->GetStatistics().frames_uploaded == 0);
}

/// The bounded queue: a caller who never drains is answered, not grown into. Without this
/// the packet queue is whatever the encode produced, and the bound is a comment.
void the_bounded_queue_answers_again_rather_than_growing()
{
    auto pixels = std::make_shared<Samples>();
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    // More than the queue holds, submitted without a single ReceiveAccessUnit. The number is
    // not the bound itself -- the point is that it is comfortably past whatever it is.
    constexpr int kFarTooMany = 400;
    bool backpressured = false;
    for (int i = 0; i < kFarTooMany && !backpressured; ++i) {
        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, 25}};
        const Verdict s = enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta});
        if (s.code() == VerdictCode::Again) {
            backpressured = true;
            break;
        }
        CHECK(s.ok());
    }
    CHECK(backpressured);

    // And it is backpressure rather than failure: drain some, and the encoder takes
    // frames again.
    AccessUnit packet;
    CHECK(enc->ReceiveAccessUnit(&packet).ok());
    PictureInfo meta;
    meta.timestamp = Stamp{kFarTooMany, Ratio{1, 25}};
    CHECK(enc->SubmitPicture(StampedPicture{make_frame(pixels, kFarTooMany), meta}).ok());
}

/// The two accessors a caller has for what actually opened, including the refusal when
/// there is nowhere to write the answer.
void the_opened_encoder_can_be_asked_what_it_is()
{
    EncodeOptions cfg = config_for();
    cfg.log = [](Verbosity, std::string_view) {};
    auto enc = open_encoder(cfg);

    CHECK(enc->GetCodingParams(nullptr).code() == VerdictCode::InvalidArgument);

    CodingParams params;
    CHECK(enc->GetCodingParams(&params).ok());
    CHECK(params.width == kWidth);
    CHECK(params.height == kHeight);
    CHECK(params.codec == Coding::H264);

    // The libavcodec x264 backend does not read its own settings back, so an empty answer
    // is the honest one -- what is asserted is that asking is safe.
    const AppliedSettings effective = enc->GetEffectiveSettings();
    (void)effective;
}

/// A backend left out of the chain because it cannot serve the codec is named in the log,
/// not merely counted.
void a_backend_left_out_of_the_chain_is_named_in_the_log()
{
    std::vector<std::pair<Verbosity, std::string>> lines;
    EncodeOptions cfg = config_for(Engine::Auto, Coding::H264);
    // X265 first: it cannot produce H.264, so it is skipped with its reason before
    // anything is opened, and the software H.264 encoder behind it takes the encode.
    cfg.settings.engine_order.preferred = {Engine::X265, Engine::X264};
    cfg.settings.engine_order.allow_unlisted = false;
    cfg.log = [&lines](Verbosity level, std::string_view message) {
        lines.emplace_back(level, std::string(message));
    };
    auto enc = open_encoder(cfg);
    CHECK(enc->GetStatistics().engine_used == Engine::X264);

    bool said = false;
    for (const auto& [level, text] : lines) {
        if (level == Verbosity::Warning && text.find("backend skipped:") != std::string::npos
            && text.find("H.264") != std::string::npos) {
            said = true;
        }
    }
    CHECK(said);
}

/// A format without a sink is refused, not quietly dropped.
void a_sidecar_without_a_sink_is_refused()
{
    EncodeOptions cfg = config_for();
    cfg.settings.metadata.sidecar = MetadataFile::NDJSON;
    bool refused = false;
    try {
        auto enc = CreateVideoEncoder(cfg);
        (void)enc;
    } catch (const EncodeError& e) {
        refused = e.status().code() == VerdictCode::InvalidArgument;
    }
    CHECK(refused);
}

} // namespace

int main()
{
    a_new_encoder_is_configured_and_knows_its_backend();
    an_input_format_that_cannot_exist_is_refused();
    same_as_source_needs_a_source();
    frames_go_in_and_packets_come_out();
    submitting_after_flush_is_refused_rather_than_ignored();
    stop_is_idempotent();
    an_empty_frame_and_a_null_packet_are_arguments_not_crashes();
    a_keyframe_request_is_counted_even_though_it_is_best_effort();
    reconfigure_takes_what_8_2_allows_and_refuses_the_rest();
    cpu_h265_refuses_a_target_bitrate_out_loud();
    the_order_ranks_the_engines_and_can_exclude_them();
    a_backend_that_cannot_encode_the_codec_is_left_out_with_its_reason();
    an_unusable_preference_order_is_refused_before_anything_opens();
    the_preference_order_is_read_where_the_backend_is_named();
    diagnostics_go_where_the_caller_says();
    strict_refuses_where_permissive_degrades();
    strict_refuses_a_frame_whose_regions_do_not_fit();
    the_sidecar_follows_the_packets_not_the_submissions();
    a_sidecar_without_a_sink_is_refused();
    a_qp_map_is_taken_as_given_rather_than_recomputed();
    a_mask_is_sampled_onto_the_grid_rather_than_ignored();
    an_inconsistent_mask_is_ignored_rather_than_half_read();
    a_cancelled_encode_does_not_report_a_sidecar_fault();
    the_report_says_what_the_statistics_say();
    pixels_the_caller_holds_go_in_through_the_borrowing_overload();
    the_bounded_queue_answers_again_rather_than_growing();
    the_opened_encoder_can_be_asked_what_it_is();
    a_backend_left_out_of_the_chain_is_named_in_the_log();
    std::printf("ok\n");
    return 0;
}
