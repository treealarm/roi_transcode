#pragma once

// A backend that does exactly what it is told to do, including nothing.
//
// The backends this project ships work: libx264 opens on any machine and encodes what it is
// given. That is what makes half of the pipeline untestable through them -- the branch taken
// when Configure declines, when a probe encodes nothing, when SubmitPicture throws, when a
// backend has no rate control of its own. Those branches run where the hardware is missing
// or busy.
//
// Shared by roi_coder_test (the pipeline) and encode_faults_test (the facade above it),
// for the reason tests/unit/enum_names.h is shared: the second copy is where the two would
// drift, and both of them are about the same interface.
//
// One object with switches rather than a family of subclasses: every one of these is the
// same backend refusing at a different point, and a class per refusal would be eight copies
// of the parts that succeed.

#include "ta_roienc/engine_catalog.h"
#include "ta_roienc/engine.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ta_roienc::testing {

class StubRun : public EngineRun {
};

/// Pixels in whatever format the backend claims to want, sized to match it. The bytes are
/// never read: what every test here asserts is that a frame arrived, not what was in it.
class StubStore : public IPictureStore {
public:
    StubStore(int32_t width, int32_t height, PixelLayout format)
        : m_width(width)
        , m_height(height)
        , m_format(format)
    {
        const std::size_t luma = static_cast<std::size_t>(width) * height;
        m_planes[0].assign(luma, uint8_t{0});
        // NV12 carries one interleaved half-height chroma plane; YUV420P carries two
        // quarter-sized ones. Both are allocated so that format() can change per test
        // without the buffer having to be rebuilt around it.
        m_planes[1].assign(luma / 2, uint8_t{128});
        m_planes[2].assign(luma / 4, uint8_t{128});
    }

    MemoryKind memory() const override { return MemoryKind::CPU; }
    int32_t width() const override { return m_width; }
    int32_t height() const override { return m_height; }
    PixelLayout format() const override { return m_format; }
    LayerView plane(int32_t index) override
    {
        const int32_t i = index < 0 || index > 2 ? 0 : index;
        const int32_t stride = i == 0 || m_format == PixelLayout::NV12 ? m_width : m_width / 2;
        return LayerView{m_planes[static_cast<std::size_t>(i)], stride};
    }

private:
    int32_t m_width;
    int32_t m_height;
    PixelLayout m_format;
    std::vector<uint8_t> m_planes[3];
};

class StubEngine : public IEngine {
public:
    // --- Geometry and what it claims it can do. ---
    int32_t width = 64;
    int32_t height = 48;
    PixelLayout pixels = PixelLayout::NV12;
    Engine reports = Engine::X265Direct;
    Coding serves = Coding::H265;
    /// Whether the backend honours a target bitrate itself. False is what puts the outer
    /// loop in charge, which is the only way to reach it: every backend in this build
    /// either has its own rate control or is guarded away from a target.
    bool native_bitrate = false;
    bool qp_offsets = false;
    uint32_t max_roi_regions = 0;

    // --- Where it refuses. Default is a backend that works. ---
    bool configure_fails = false;
    bool needs_probe = false;
    bool probe_encodes_nothing = false;
    /// The second Configure -- the untouched session opened after a successful probe.
    bool reopen_fails = false;
    bool no_codec_parameters = false;
    bool create_frame_fails = false;
    bool submit_fails = false;
    /// Throws instead of returning a status. A backend on its own SDK is C++ underneath,
    /// and the facade's promise is that an exception becomes a Verdict rather than
    /// travelling to the caller.
    bool submit_throws = false;
    bool flush_fails = false;
    /// ReceiveAccessUnit answers with a real error rather than Again: the branch that must
    /// reach the caller instead of ending the drain quietly.
    bool receive_fails = false;
    /// Packets with neither duration nor time base, the way libx265 and the hardware
    /// wrappers hand them over.
    bool packets_are_bare = false;
    /// Added to every packet's timestamp, so that a record keyed on the submitted pts has
    /// no packet to belong to -- the sidecar's orphan.
    int64_t pts_skew = 0;
    /// What Statistics reports for the per-block map running out of scale.
    long quantiser_clamped = 0;
    /// Bytes each packet claims to carry. The outer bitrate loop reads exactly this, so it
    /// is what makes a stream "over target" without encoding anything.
    std::size_t packet_bytes = 1;

    // --- How often each was called, for the assertions about sequence. ---
    mutable int configures = 0;
    mutable int submits = 0;

    Engine kind() const override { return reports; }

    EngineAbilities GetCapabilities(const AbilityQuery& query) const override
    {
        (void)query;
        EngineAbilities caps;
        caps.h264 = serves == Coding::H264;
        caps.h265 = serves == Coding::H265;
        caps.native_bitrate = native_bitrate;
        caps.qp_offsets = qp_offsets;
        caps.roi_areas = true;
        caps.max_roi_regions = max_roi_regions;
        return caps;
    }

    Fallible<TranscoderPtr> Configure(const EngineSetup& config) const override
    {
        (void)config;
        ++configures;
        const bool refuse = configures == 1 ? configure_fails : reopen_fails;
        if (refuse) {
            return Verdict::DeviceUnavailable("the fake backend was told to decline");
        }
        return TranscoderPtr(new StubRun());
    }

    Fallible<PictureStorePtr> CreatePicture(EngineRun& session, MemoryKind memory)
        const override
    {
        (void)session;
        (void)memory;
        if (create_frame_fails) {
            return Verdict::Unsupported("the fake backend hands out no frames");
        }
        return PictureStorePtr(new StubStore(width, height, pixels));
    }

    Verdict SubmitPicture(EngineRun& session, IPictureStore& frame,
        const EnginePictureControl& control) const override
    {
        (void)session;
        (void)frame;
        ++submits;
        if (submit_throws) {
            throw std::runtime_error("the fake backend threw on a frame");
        }
        if (submit_fails) {
            return Verdict::Error("the fake backend refused the frame");
        }
        if (!probe_encodes_nothing) {
            m_queued.push_back(control.pts + pts_skew);
        }
        return Verdict::Ok();
    }

    Fallible<AccessUnit> ReceiveAccessUnit(EngineRun& session) const override
    {
        (void)session;
        if (receive_fails) {
            return Verdict::Error("the fake backend lost the bitstream");
        }
        if (m_queued.empty()) {
            return Verdict::Again();
        }
        const int64_t pts = m_queued.front();
        m_queued.erase(m_queued.begin());

        AccessUnit packet;
        packet.payload = AccessUnitData(std::vector<uint8_t>(packet_bytes, uint8_t{0}));
        packet.pts = pts;
        packet.dts = pts;
        if (packets_are_bare) {
            // Neither field set: duration zero and a time base with no denominator are
            // what libx265 and the hardware wrappers hand over.
            packet.time_base = Ratio{0, 0};
        } else {
            packet.duration = kTicksPerFrame;
            packet.time_base = Ratio{1, kTimeBaseDen};
        }
        return packet;
    }

    Verdict Flush(EngineRun& session) const override
    {
        (void)session;
        return flush_fails ? Verdict::Error("the fake backend refused to flush")
                           : Verdict::Ok();
    }

    Fallible<CodingParams> GetCodingParams(EngineRun& session) const override
    {
        (void)session;
        if (no_codec_parameters) {
            return Verdict::Error("the fake backend cannot describe itself");
        }
        CodingParams params;
        params.codec = serves;
        params.width = width;
        params.height = height;
        params.time_base = Ratio{1, kTimeBaseDen};
        return params;
    }

    EngineCounters Statistics(const EngineRun& session) const override
    {
        (void)session;
        EngineCounters stats;
        stats.frames_quantiser_clamped = quantiser_clamped;
        return stats;
    }

    bool NeedsProbe() const override { return needs_probe; }

    /// The time base this backend reports, and the frame rate a test configures against it:
    /// 90 kHz over 30 fps is 3000 ticks per frame, which is what a duration fallback has to
    /// arrive at.
    static constexpr int32_t kTimeBaseDen = 90000;
    static constexpr int32_t kFrameRate = 30;
    static constexpr int64_t kTicksPerFrame = kTimeBaseDen / kFrameRate;

private:
    mutable std::vector<int64_t> m_queued;
};

/// A registry holding nothing but the fake. Local to each case: two cases configuring the
/// same backend differently must not be able to see each other's.
inline EngineCatalog registry_with(const StubEngine& backend)
{
    EngineCatalog registry;
    if (!registry.Register(backend).ok()) {
        throw std::runtime_error("the fake backend would not register");
    }
    return registry;
}

} // namespace ta_roienc::testing
