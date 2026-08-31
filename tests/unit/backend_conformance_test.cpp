// AT-002, AT-003, AT-006 and AT-007 -- what every backend has to do -- TZ v4.1 §48.
//
// The specification says "каждый certified backend" and then lists encode, decode, frame
// count, PTS and EOS. Until now those were asserted once, against whichever backend the
// test happened to open, which is a different claim: it says the library works, not that a
// backend does. This runs the same standard input through every backend in the build and
// checks the same answers.
//
// The expected output is properties, not bytes. Two encoders never produce the same
// bitstream, and pinning one would only record which encoder was installed the day it was
// written. What can be pinned exactly is what the library promises: every frame in comes
// out as packets, the picture decodes, the count matches, the timestamps are the ones that
// went in, and end-of-stream arrives once. A backend that fails any of those is broken
// however good its pictures look.
//
// Decoding matters and is not decoration. A backend emitting well-formed-looking packets
// full of nonsense passes a packet count and fails here, which is the whole reason §48
// spells "decode" beside "encode".
//
// A backend whose device is absent is a SKIP with the reason it gave -- never a silent
// pass. §50 forbids "catch, continue, PASS", and an unauditable skip is that in a hat.

#include "sve/encoder.h"
#include "sve/frame.h"

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

using namespace sve;

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
// matrix runs in seconds. §48 does not fix a count for AT-002/003; what it fixes is that
// the count coming out equals the count going in.
constexpr int kFrames = 60;
// 640x480 rather than something tiny, for the reason AT-009 found the hard way: Quick Sync
// refuses 128x96 outright, and a backend that skips because of the test's own frame size
// has not been tested.
constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;
constexpr int kTimeBase = 25;

struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

/// Moving content, because a flat grey clip is the one input where a broken encoder still
/// looks right: everything predicts perfectly and nothing has to be coded.
void draw(Pixels& p, int frame)
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

EncoderConfig config_for(Backend backend, Codec codec)
{
    EncoderConfig cfg;
    cfg.input.width = kWidth;
    cfg.input.height = kHeight;
    cfg.input.frame_rate = Rational{kTimeBase, 1};
    cfg.input.time_base = Rational{1, kTimeBase};
    cfg.input.pixel_format = PixelFormat::YUV420P;
    cfg.settings.encoder = backend;
    cfg.settings.codec = codec;
    cfg.settings.crf = 30;
    cfg.settings.metadata.bitstream_sei = false;
    cfg.log = [](LogLevel, std::string_view) {};
    return cfg;
}

Frame make_frame(const std::shared_ptr<Pixels>& pixels, int64_t pts)
{
    RawFrameView view;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.format.pixel_format = PixelFormat::YUV420P;
    view.format.frame_rate = Rational{kTimeBase, 1};
    view.format.time_base = Rational{1, kTimeBase};
    view.plane_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.timestamp = MediaTimestamp{pts, Rational{1, kTimeBase}};
    view.owner = pixels;

    Frame frame;
    check(WrapRawFrame(view, &frame).ok(), "WrapRawFrame");
    return frame;
}

/// Decodes what the encoder produced and returns the number of pictures that came out.
/// Negative means the bitstream could not be decoded at all, which is the failure this
/// exists to catch.
int decode_count(Codec codec, const CodecParameters& params,
    const std::vector<std::vector<uint8_t>>& packets)
{
    const AVCodec* dec = avcodec_find_decoder(
        codec == Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
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

/// AT-002 / AT-003 / AT-006 for one backend and one codec, or the reason it could not run.
void conformance(Backend backend, Codec codec, const char* label)
{
    std::unique_ptr<SmartVideoEncoder> enc;
    try {
        enc = CreateVideoEncoder(config_for(backend, codec));
    } catch (const EncoderError& e) {
        // The device is absent, or this backend cannot make this codec. Both are a
        // documented SKIP, and what the backend said is the evidence.
        std::printf("  skip  %-18s %s\n", label, e.what());
        return;
    }

    auto pixels = std::make_shared<Pixels>();
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<int64_t> out_pts;
    int64_t last_dts = INT64_MIN;
    bool dts_monotonic = true;

    const auto collect = [&]() {
        for (;;) {
            EncodedPacket packet;
            const Status s = enc->ReceivePacket(&packet);
            if (s.code() == StatusCode::Again || s.code() == StatusCode::EndOfStream) {
                return s.code();
            }
            if (!s.ok()) {
                check(false, std::string(label) + ": ReceivePacket -- " + s.message());
                return StatusCode::Error;
            }
            const std::span<const uint8_t> bytes = packet.payload.bytes();
            payloads.emplace_back(bytes.begin(), bytes.end());
            out_pts.push_back(packet.pts);
            // Decode order, which is what §21 requires of a backend: DTS never goes
            // backwards even where PTS does, and PTS does as soon as B-frames exist.
            if (packet.dts != kNoTimestamp) {
                if (packet.dts < last_dts) {
                    dts_monotonic = false;
                }
                last_dts = packet.dts;
            }
        }
    };

    for (int i = 0; i < kFrames; ++i) {
        draw(*pixels, i);
        FrameMetadata meta;
        meta.timestamp = MediaTimestamp{i, Rational{1, kTimeBase}};
        Status s = enc->SubmitFrame(TimedFrame{make_frame(pixels, i), meta});
        while (s.code() == StatusCode::Again) {
            collect();
            s = enc->SubmitFrame(TimedFrame{make_frame(pixels, i), meta});
        }
        check(s.ok(), std::string(label) + ": SubmitFrame -- " + s.message());
        collect();
    }

    check(enc->Flush().ok(), std::string(label) + ": Flush");
    const StatusCode ended = collect();
    // AT-002/003: end of stream, said once and said explicitly. A drain that just stops
    // producing packets is indistinguishable from one that failed.
    check(ended == StatusCode::EndOfStream, std::string(label) + ": drain reaches EndOfStream");

    EncodedPacket after;
    check(enc->ReceivePacket(&after).code() == StatusCode::EndOfStream,
        std::string(label) + ": EndOfStream is stable once reached");

    const EncoderStatistics stats = enc->GetStatistics();
    check(stats.frames_submitted == kFrames,
        std::string(label) + ": every frame was accepted");

    // AT-006, metadata mapping at 100%: every timestamp submitted comes back exactly once.
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

    // AT-002/003: the picture decodes, and the count matches.
    CodecParameters params;
    check(enc->GetCodecParameters(&params).ok(), std::string(label) + ": GetCodecParameters");
    check(params.width == kWidth && params.height == kHeight,
        std::string(label) + ": codec parameters carry the frame size");
    const int decoded = decode_count(codec, params, payloads);
    check(decoded == kFrames,
        std::string(label) + ": decodes to " + std::to_string(kFrames) + " pictures (got "
            + std::to_string(decoded) + ")");

    std::printf("  ok    %-18s %d frames in, %zu packets out, %d decoded, %s\n", label,
        kFrames, payloads.size(), decoded, backend_name(stats.encoder_used));
}

/// AT-007: asking for something a backend cannot do is answered, never absorbed.
///
/// Strict is the half that can be asserted without knowing which hardware is present:
/// §22 says Permissive may degrade provided it says so, and Strict must refuse. What must
/// never happen on either is silence, so the log is watched as well as the status.
void at_007_capability_fallback(Backend backend, Codec codec, const char* label)
{
    std::vector<std::string> lines;
    EncoderConfig cfg = config_for(backend, codec);
    cfg.settings.fallback = FallbackPolicy::Strict;
    // A bitrate target on CPU HEVC is the one capability gap this build is certain to
    // have; on backends that support it, nothing trips and the encoder simply opens.
    cfg.settings.rate_control.target_bitrate = 800000;
    cfg.log = [&lines](LogLevel, std::string_view m) { lines.emplace_back(m); };

    try {
        const auto enc = CreateVideoEncoder(cfg);
        // It opened, so nothing was refused. Then nothing may have been quietly dropped
        // either: a degradation without a word in the log is what §50 calls a silent
        // fallback and what AT-007 marks FAIL.
        const EncoderStatistics stats = enc->GetStatistics();
        if (stats.fallbacks_applied > 0) {
            check(!lines.empty(),
                std::string(label) + ": a fallback was applied and said so");
        }
        std::printf("  ok    %-18s honoured the request (%lld fallbacks, %zu log lines)\n",
            label, static_cast<long long>(stats.fallbacks_applied), lines.size());
    } catch (const EncoderError& e) {
        // Refused, which under Strict is the correct answer. The code has to be one that
        // tells the caller what to do about it.
        const StatusCode code = e.code();
        check(code == StatusCode::Unsupported || code == StatusCode::DeviceUnavailable
                || code == StatusCode::InvalidArgument,
            std::string(label) + ": refusal carries an actionable code");
        std::printf("  ok    %-18s refused explicitly: %s\n", label, e.what());
    }
}

struct Candidate {
    Backend backend;
    const char* name;
};

} // namespace

int main()
{
    // Every backend this build has. Absent hardware turns into a SKIP inside, so the list
    // is the same on a laptop and on a machine with three GPUs -- what differs is how much
    // of it runs, and the output says which.
    const Candidate candidates[] = {
        {Backend::X264, "x264"},
        {Backend::X265, "x265"},
        {Backend::IntelOneVpl, "onevpl"},
        {Backend::Vaapi, "vaapi"},
        {Backend::NvidiaNvenc, "nvenc"},
    };

    std::printf("AT-002 (H.264: encode, decode, frame count, PTS, EOS):\n");
    for (const Candidate& c : candidates) {
        conformance(c.backend, Codec::H264, c.name);
    }

    std::printf("AT-003 (H.265: the same):\n");
    for (const Candidate& c : candidates) {
        conformance(c.backend, Codec::H265, c.name);
    }

    std::printf("AT-007 (capability fallback is answered, never absorbed):\n");
    for (const Candidate& c : candidates) {
        at_007_capability_fallback(c.backend, Codec::H265, c.name);
    }

    if (failures > 0) {
        std::fprintf(stderr, "backend_conformance_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
