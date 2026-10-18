// What every backend has to do: encode, decode, frame count, timestamps and end of stream.
//
// Asserted once against whichever backend a test happens to open, those say the library
// works, not that a backend does. This runs the same standard input through every backend
// in the build and checks the same answers.
//
// The expected output is properties, not bytes. Two encoders never produce the same
// bitstream, and pinning one would only record which encoder was installed the day it was
// written. What can be pinned exactly is what the library promises: every frame in comes
// out as packets, the picture decodes, the count matches, the timestamps are the ones that
// went in, and end-of-stream arrives once. A backend that fails any of those is broken
// however good its pictures look.
//
// Decoding matters and is not decoration. A backend emitting well-formed-looking packets
// full of nonsense passes a packet count and fails here.
//
// A backend whose device is absent is a SKIP with the reason it gave -- never a silent
// pass: "catch, continue, PASS" with an unauditable skip is a silent fallback in a hat.

#include "ta_roienc/encode.h"
#include "ta_roienc/picture.h"
#include "ta_roienc/metadata_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace ta_roienc;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

// Enough frames for lookahead and B-frames to reorder something, few enough that the whole
// matrix runs in seconds. What matters is that the count coming out equals the count going
// in.
constexpr int kFrames = 60;
// 640x480 rather than something tiny: Quick Sync refuses 128x96 outright, and a backend that
// skips because of the test's own frame size has not been tested.
constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;
constexpr int kTimeBase = 25;

struct Samples {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

/// Moving content, because a flat grey clip is the one input where a broken encoder still
/// looks right: everything predicts perfectly and nothing has to be coded.
void draw(Samples& p, int frame)
{
    const int x0 = (frame * 7) % (kWidth - 64);
    const int y0 = (frame * 3) % (kHeight - 64);
    std::fill(p.y.begin(), p.y.end(), uint8_t{40});
    for (int y = y0; y < y0 + 64; ++y) {
        for (int x = x0; x < x0 + 64; ++x) {
            p.y[static_cast<std::size_t>(y) * kWidth + x] = 220;
        }
    }
}

EncodeOptions config_for(Engine backend, Coding codec)
{
    EncodeOptions cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Ratio{kTimeBase, 1};
    cfg.input.time_base = Ratio{1, kTimeBase};
    cfg.input.pixel_layout = PixelLayout::YUV420P;
    cfg.settings.encoder = backend;
    cfg.settings.codec = codec;
    cfg.settings.crf = 30;
    // On, and on deliberately. It was off here until a submitted SEI payload turned out to
    // outlive the call that submitted it -- x264 writes it when the frame is finally coded,
    // which with lookahead is several calls later -- and the crash landed in the file
    // pipeline rather than in this suite, because this suite was not asking for any SEI.
    // A conformance test that skips the metadata path does not cover the metadata path.
    cfg.settings.metadata.bitstream_sei = true;
    cfg.settings.quality.kinds["plate"] = ClassQuality{-10, 0.02};
    cfg.log = [](Verbosity, std::string_view) {};
    return cfg;
}

Picture make_frame(const std::shared_ptr<Samples>& pixels, int64_t pts)
{
    PictureView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_layout = PixelLayout::YUV420P;
    view.format.frame_rate = Ratio{kTimeBase, 1};
    view.format.time_base = Ratio{1, kTimeBase};
    view.layer_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.timestamp = Stamp{pts, Ratio{1, kTimeBase}};
    view.owner = pixels;

    Picture frame;
    check(WrapPicture(view, &frame).ok(), "WrapPicture");
    return frame;
}

/// Decodes what the encoder produced and returns the number of pictures that came out.
/// Negative means the bitstream could not be decoded at all, which is the failure this
/// exists to catch.
int decode_count(Coding codec, const CodingParams& params,
    const std::vector<std::vector<uint8_t>>& packets)
{
    const AVCodec* dec = avcodec_find_decoder(
        codec == Coding::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    if (dec == nullptr) {
        return -1;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (ctx == nullptr) {
        return -1;
    }
    // extradata carries SPS/PPS/VPS for a backend that keeps them out of band; without it
    // such a stream decodes to nothing and the test would blame the encoder.
    const std::span<const uint8_t> extra = params.extradata.bytes();
    if (!extra.empty()) {
        ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (ctx->extradata == nullptr) {
            avcodec_free_context(&ctx);
            return -1;
        }
        std::copy(extra.begin(), extra.end(), ctx->extradata);
        ctx->extradata_size = static_cast<int>(extra.size());
    }
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    int pictures = 0;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* got = av_frame_alloc();
    const auto drain = [&]() {
        while (avcodec_receive_frame(ctx, got) == 0) {
            ++pictures;
        }
    };
    for (const std::vector<uint8_t>& bytes : packets) {
        if (av_new_packet(pkt, static_cast<int>(bytes.size())) < 0) {
            break;
        }
        std::copy(bytes.begin(), bytes.end(), pkt->data);
        if (avcodec_send_packet(ctx, pkt) < 0) {
            av_packet_unref(pkt);
            pictures = -1;
            break;
        }
        av_packet_unref(pkt);
        drain();
    }
    if (pictures >= 0) {
        avcodec_send_packet(ctx, nullptr);
        drain();
    }
    av_frame_free(&got);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return pictures;
}

/// Our object metadata, read back out of the bitstream the backend produced.
///
/// The frames carry a varying number of objects, because the same single box on every
/// frame makes every SEI payload the same length, which is exactly the case that hides a
/// decoder or encoder mishandling a payload whose size changes. x265 4.1 sizes its
/// per-frame user-SEI buffer from the first payload a pooled frame ever sees and never
/// resizes it, so a constant payload is the one input that cannot catch it.
int count_our_sei(const std::vector<std::vector<uint8_t>>& payloads, Coding codec, int* bad)
{
    const std::span<const uint8_t, 16> uuid = sei_identifier();
    // H.264 prefix SEI is nal_unit_type 6 in a one-byte header; HEVC is 39 in a two-byte
    // one. Everything after the header is RBSP, escape bytes and all.
    const int header_bytes = codec == Coding::H265 ? 2 : 1;
    int found = 0;
    for (const std::vector<uint8_t>& packet : payloads) {
        for (std::size_t i = 0; i + 4 < packet.size(); ++i) {
            if (packet[i] != 0 || packet[i + 1] != 0 || packet[i + 2] != 1) {
                continue;
            }
            const std::size_t nal = i + 3;
            const int type = codec == Coding::H265 ? ((packet[nal] >> 1) & 0x3F)
                                                  : (packet[nal] & 0x1F);
            if (type != (codec == Coding::H265 ? 39 : 6)) {
                continue;
            }
            // Undo emulation prevention before reading anything: a 0x03 inside our own
            // payload would otherwise shift every following byte.
            std::vector<uint8_t> rbsp;
            int zeros = 0;
            for (std::size_t j = nal + header_bytes; j < packet.size(); ++j) {
                if (zeros == 2 && packet[j] == 0x03) {
                    zeros = 0;
                    continue;
                }
                if (zeros == 2 && packet[j] <= 0x01) {
                    break; // the next start code
                }
                rbsp.push_back(packet[j]);
                zeros = packet[j] == 0 ? zeros + 1 : 0;
            }
            std::size_t p = 0;
            while (p < rbsp.size() && rbsp[p] != 0x80) {
                int payload_type = 0;
                while (p < rbsp.size() && rbsp[p] == 0xFF) { payload_type += 255; ++p; }
                if (p >= rbsp.size()) break;
                payload_type += rbsp[p++];
                std::size_t payload_size = 0;
                while (p < rbsp.size() && rbsp[p] == 0xFF) { payload_size += 255; ++p; }
                if (p >= rbsp.size()) break;
                payload_size += rbsp[p++];
                if (p + payload_size > rbsp.size()) break;
                const std::span<const uint8_t> body(rbsp.data() + p, payload_size);
                p += payload_size;
                if (payload_type != 5 || body.size() < 16
                    || !std::equal(uuid.begin(), uuid.end(), body.begin())) {
                    continue;
                }
                ++found;
                SeiRecord decoded;
                // The CRC is checked inside, so this failing means the bytes we wrote are
                // not the bytes a reader gets -- a NAL written wrong, or a payload cut.
                if (!read_sei_record(body, decoded)) {
                    ++*bad;
                }
            }
        }
    }
    return found;
}

/// Encode, decode, count and timestamps for one backend and one codec, or the reason it
/// could not run.
void conformance(Engine backend, Coding codec, const char* label)
{
    std::unique_ptr<VideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(config_for(backend, codec));
    } catch (const EncodeError& e) {
        // The device is absent, or this backend cannot make this codec. Both are a
        // documented SKIP, and what the backend said is the evidence.
        std::printf("  skip  %-18s %s\n", label, e.what());
        return;
    }

    auto pixels = std::make_shared<Samples>();
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<int64_t> out_pts;
    int64_t last_dts = INT64_MIN;
    bool dts_monotonic = true;

    const auto collect = [&]() {
        for (;;) {
            AccessUnit packet;
            const Verdict s = enc->ReceiveAccessUnit(&packet);
            if (s.code() == VerdictCode::Again || s.code() == VerdictCode::EndOfStream) {
                return s.code();
            }
            if (!s.ok()) {
                check(false, std::string(label) + ": ReceiveAccessUnit -- " + s.message());
                return VerdictCode::Error;
            }
            const std::span<const uint8_t> bytes = packet.payload.bytes();
            payloads.emplace_back(bytes.begin(), bytes.end());
            out_pts.push_back(packet.pts);
            // Decode order: DTS never goes backwards even where PTS does, and PTS does as
            // soon as B-frames exist.
            if (packet.dts != kNoTimestamp) {
                if (packet.dts < last_dts) {
                    dts_monotonic = false;
                }
                last_dts = packet.dts;
            }
        }
    };

    // Boxes that move with the drawn square, so every frame carries regions: without them
    // the encoder submits an empty region list and the whole adaptive path -- the quant
    // offsets, the SEI, the thing this library is for -- goes untested.
    //
    // How many of them changes from frame to frame, and that is not decoration. A fixed
    // count makes every SEI payload the same length, and a payload that never changes
    // length is the one input that cannot catch a buffer sized once and reused -- which is
    // exactly what x265 4.1 does with user SEI. This ran green against that bug for as
    // long as it submitted one box.
    std::vector<Box> boxes;
    for (int i = 0; i < kFrames; ++i) {
        draw(*pixels, i);
        boxes.assign(static_cast<std::size_t>(1 + (i % 4)), Box{});
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            const int nudge = static_cast<int>(b) * 24;
            boxes[b].kind = "plate";
            boxes[b].x = static_cast<float>((i * 7 + nudge) % (kWidth - 64) + 32) / kWidth;
            boxes[b].y = static_cast<float>((i * 3 + nudge) % (kHeight - 64) + 32) / kHeight;
            boxes[b].width = 64.0f / kWidth;
            boxes[b].height = 64.0f / kHeight;
            boxes[b].confidence = 0.9f;
            boxes[b].track_id = static_cast<uint64_t>(b + 1);
        }

        PictureInfo meta;
        meta.timestamp = Stamp{i, Ratio{1, kTimeBase}};
        meta.regions.boxes = boxes;
        Verdict s = enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta});
        while (s.code() == VerdictCode::Again) {
            collect();
            s = enc->SubmitPicture(StampedPicture{make_frame(pixels, i), meta});
        }
        check(s.ok(), std::string(label) + ": SubmitPicture -- " + s.message());
        collect();
    }

    check(enc->Flush().ok(), std::string(label) + ": Flush");
    const VerdictCode ended = collect();
    // End of stream, said once and said explicitly. A drain that just stops producing
    // packets is indistinguishable from one that failed.
    check(ended == VerdictCode::EndOfStream, std::string(label) + ": drain reaches EndOfStream");

    AccessUnit after;
    check(enc->ReceiveAccessUnit(&after).code() == VerdictCode::EndOfStream,
        std::string(label) + ": EndOfStream is stable once reached");

    const Counters stats = enc->GetStatistics();
    check(stats.frames_submitted == kFrames,
        std::string(label) + ": every frame was accepted");

    // Metadata mapping at 100%: every timestamp submitted comes back exactly once.
    // Compared as a set, because with B-frames the order is the encoder's business and
    // insisting on it would be asserting a coding decision rather than a contract.
    check(out_pts.size() == static_cast<std::size_t>(kFrames),
        std::string(label) + ": one packet per frame ("
            + std::to_string(out_pts.size()) + " of " + std::to_string(kFrames) + ")");
    std::vector<int64_t> sorted = out_pts;
    std::sort(sorted.begin(), sorted.end());
    bool pts_complete = sorted.size() == static_cast<std::size_t>(kFrames);
    for (std::size_t i = 0; pts_complete && i < sorted.size(); ++i) {
        pts_complete = sorted[i] == static_cast<int64_t>(i);
    }
    check(pts_complete, std::string(label) + ": every submitted PTS came back exactly once");
    check(dts_monotonic, std::string(label) + ": DTS never goes backwards");

    // The picture decodes, and the count matches.
    CodingParams params;
    check(enc->GetCodingParams(&params).ok(), std::string(label) + ": GetCodingParams");
    check(params.width == kWidth && params.height == kHeight,
        std::string(label) + ": codec parameters carry the frame size");
    const int decoded = decode_count(codec, params, payloads);
    check(decoded == kFrames,
        std::string(label) + ": decodes to " + std::to_string(kFrames) + " pictures (got "
            + std::to_string(decoded) + ")");

    // The metadata a caller submitted has to come back out of the bitstream, once per
    // frame, and parse. A backend that carries no SEI at all -- the guard turns it off for
    // CPU H.265 -- says so here rather than being asserted against, because "off" and
    // "broken" must not print the same line.
    int undecodable = 0;
    const int sei_found = count_our_sei(payloads, codec, &undecodable);
    if (sei_found > 0) {
        check(sei_found == kFrames,
            std::string(label) + ": one metadata SEI per frame (got "
                + std::to_string(sei_found) + " of " + std::to_string(kFrames) + ")");
        check(undecodable == 0,
            std::string(label) + ": every metadata SEI parses (" + std::to_string(undecodable)
                + " did not)");
    }
    const std::string sei_note =
        sei_found > 0 ? std::to_string(sei_found) + " sei" : std::string("no sei");

    std::printf("  ok    %-18s %d frames in, %zu packets out, %d decoded, %s, %s\n", label,
        kFrames, payloads.size(), decoded, sei_note.c_str(),
        engine_name(stats.engine_used));
}

/// Asking for something a backend cannot do is answered, never absorbed.
///
/// Strict is the half that can be asserted without knowing which hardware is present:
/// Permissive may degrade provided it says so, and Strict must refuse. What must never
/// happen on either is silence, so the log is watched as well as the status.
void capability_fallback_is_answered(Engine backend, Coding codec, const char* label)
{
    std::vector<std::string> lines;
    EncodeOptions cfg = config_for(backend, codec);
    cfg.settings.fallback = EngineFallback::Strict;
    // A bitrate target on CPU HEVC is the one capability gap this build is certain to
    // have; on backends that support it, nothing trips and the encoder simply opens.
    cfg.settings.rate_control.target_bitrate = 800000;
    cfg.log = [&lines](Verbosity, std::string_view m) { lines.emplace_back(m); };

    try {
        const auto enc = CreateVideoEncoder(cfg);
        // It opened, so nothing was refused. Then nothing may have been quietly dropped
        // either: a degradation without a word in the log is a silent fallback.
        const Counters stats = enc->GetStatistics();
        if (stats.fallbacks_applied > 0) {
            check(!lines.empty(),
                std::string(label) + ": a fallback was applied and said so");
        }
        // An encoder that opened while unable to honour what was asked has to have counted
        // a fallback; otherwise the request was neither met nor refused. A guard keyed on
        // the backend rather than its capability would let X265Direct through here,
        // silently, and this line is what says so.
        if (!enc->GetCapabilities().native_bitrate) {
            check(stats.fallbacks_applied > 0,
                std::string(label) + ": a bitrate target it cannot honour was answered");
        }
        std::printf("  ok    %-18s honoured the request (%lld fallbacks, %zu log lines)\n",
            label, static_cast<long long>(stats.fallbacks_applied), lines.size());
    } catch (const EncodeError& e) {
        // Refused, which under Strict is the correct answer. The code has to be one that
        // tells the caller what to do about it.
        const VerdictCode code = e.code();
        check(code == VerdictCode::Unsupported || code == VerdictCode::DeviceUnavailable
                || code == VerdictCode::InvalidArgument,
            std::string(label) + ": refusal carries an actionable code");
        std::printf("  ok    %-18s refused explicitly: %s\n", label, e.what());
    }
}

struct Contender {
    Engine backend;
    const char* name;
};

/// NV12 is an input format, so every backend has to give an answer to one: encode it, or
/// refuse it with Unsupported. What none of them may do is take it and then read a plane
/// it does not have -- every other test here builds YUV420P.
void nv12_input_is_answered_and_never_crashes(Engine backend, const char* label)
{
    EncodeOptions cfg = config_for(backend, Coding::H264);
    cfg.input.pixel_layout = PixelLayout::NV12;

    std::unique_ptr<VideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(cfg);
    } catch (const std::exception& e) {
        // No device, or the backend refuses NV12 at open. Both are answers.
        std::printf("  skip  %s: %s\n", label, e.what());
        return;
    }

    // Two planes, which is what NV12 is. A backend that reaches for a third finds nothing.
    auto luma = std::make_shared<std::vector<uint8_t>>(
        static_cast<std::size_t>(kWidth) * kHeight, 128);
    auto chroma = std::make_shared<std::vector<uint8_t>>(
        static_cast<std::size_t>(kWidth) * ((kHeight + 1) / 2), 128);

    PictureView view;
    view.format = cfg.input;
    view.layer_count = 2;
    view.planes[0] = {luma->data(), kWidth, luma->size()};
    view.planes[1] = {chroma->data(), kWidth, chroma->size()};
    view.timestamp = Stamp{0, Ratio{1, kTimeBase}};
    view.owner = luma;

    Picture frame;
    check(WrapPicture(view, &frame).ok(), std::string(label) + ": WrapPicture(NV12)");

    PictureInfo meta;
    meta.timestamp = view.timestamp;
    const Verdict s = enc->SubmitPicture(StampedPicture{std::move(frame), meta});
    check(s.ok() || s.code() == VerdictCode::Unsupported || s.code() == VerdictCode::Again,
        std::string(label) + ": NV12 is encoded or refused, not mishandled -- " + s.message());
    std::printf("  ok    %s: %s\n", label, s.ok() ? "encoded" : s.message().c_str());
}

} // namespace

int main()
{
    // Every backend this build has. Absent hardware turns into a SKIP inside, so the list
    // is the same on a laptop and on a machine with three GPUs -- what differs is how much
    // of it runs, and the output says which.
    const Contender candidates[] = {
        {Engine::X264, "x264"},
        {Engine::X264Direct, "x264direct"},
        {Engine::X265, "x265"},
        {Engine::X265Direct, "x265direct"},
        {Engine::IntelOneVpl, "onevpl"},
        {Engine::IntelOneVplDirect, "onevpldirect"},
        {Engine::Vaapi, "vaapi"},
    };

    std::printf("H.264: encode, decode, frame count, PTS, EOS:\n");
    for (const Contender& c : candidates) {
        conformance(c.backend, Coding::H264, c.name);
    }

    std::printf("H.265: the same:\n");
    for (const Contender& c : candidates) {
        conformance(c.backend, Coding::H265, c.name);
    }

    std::printf("capability fallback is answered, never absorbed:\n");
    for (const Contender& c : candidates) {
        capability_fallback_is_answered(c.backend, Coding::H265, c.name);
    }

    std::printf("NV12 in: answered, never mishandled:\n");
    for (const Contender& c : candidates) {
        nv12_input_is_answered_and_never_crashes(c.backend, c.name);
    }

    if (failures > 0) {
        std::fprintf(stderr, "backend_conformance_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
