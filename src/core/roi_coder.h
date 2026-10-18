#pragma once

// The encoder with region-of-interest side data, independent of where the frames came
// from. A file pipeline and a caller pushing frames want exactly this and nothing else
// around it.
//
// What lives here is backend selection: walking the fallback chain, honouring a probe, and
// reporting which one opened. The frames themselves go through IEngine.

#include "ta_roienc/engine_catalog.h"
#include "ta_roienc/engine.h"
#include "ta_roienc/basics.h"

#include <span>
#include <string>
#include <vector>

namespace ta_roienc {

class AccessUnitOutlet;

class RoiCoder {
public:
    /// Which deployment's backends this encoder may open, defaulting to the process-wide
    /// registry -- so the ordinary caller constructs it with nothing.
    ///
    /// A constructor argument rather than a global lookup inside open_deferred, so that a
    /// caller assembling its own EngineCatalog can reach this pipeline: everything else
    /// this class needs arrives through IEngine or AccessUnitOutlet too.
    ///
    /// **The registry must outlive the encoder.** It is held by pointer, not copied: a
    /// EngineCatalog holds references to backends that are stateless immortal singletons,
    /// so copying it would be copying a list of pointers and would say nothing about
    /// lifetime anyway. A temporary binds here without a diagnostic, and today survives
    /// only by accident -- open_deferred is the sole reader, and CreateVideoEncoderIn calls
    /// it inside the full-expression that keeps a temporary alive. The two-step form below
    /// has no such luck.
    explicit RoiCoder(const EngineCatalog& registry = default_engine_catalog())
        : m_registry(&registry)
    {
    }

    // Opens a backend, falling back down the chain when kind is Auto, then
    // calls sink.begin(). Throws if nothing opens.
    void open(const EngineSetup& config, AccessUnitOutlet& sink);

    // The same in two steps, for a caller that must know which backend opened
    // before the sink is told about it.
    void open_deferred(const EngineSetup& config);
    void attach(AccessUnitOutlet& sink);

    Engine kind() const { return m_kind; }
    /// Backends that were tried and declined before one opened. Normal on a machine
    /// without the hardware; the choice should be observable rather than inferred.
    long backends_declined() const { return m_backends_declined; }
    /// Backends left out of the chain before anything was opened, one reason each -- a
    /// backend that cannot produce the requested codec. Kept separate from the declined
    /// count because these were never tried: no device was touched.
    const std::vector<std::string>& backends_skipped() const { return m_backends_skipped; }
    /// What the backend that actually opened is able to do. Only meaningful after open;
    /// before that there is no backend to ask.
    EngineAbilities capabilities() const;
    /// What the opened backend settled on. Empty before open, and empty afterwards from a
    /// backend that cannot read its own settings back.
    AppliedSettings effective_settings() const;
    /// The hardware device the opened backend is using. Empty before open, and empty from
    /// any backend that encodes on the CPU.
    NativeHandle hardware_device() const;
    // Human-readable name of the backend that actually opened, codec included.
    const char* label() const;
    /// What the opened encoder produces -- codec, geometry, time base, extradata. Read
    /// from the backend once at open, because that is the only thing every backend can
    /// answer; there is no AVCodecContext here to reach into.
    const CodingParams& codec_parameters() const { return m_params; }

    /// How many frames needed a pixel-format conversion or an upload to the GPU before the
    /// encoder would take them. Both zero is what zero-copy means, and the only way to
    /// tell a factory frame that avoided the copy from one that merely moved it.
    long frames_converted() const { return statistics().frames_converted; }
    long frames_uploaded() const { return statistics().frames_uploaded; }
    /// Frames whose per-block quantiser map wanted a value outside what the codec has.
    long frames_quantiser_clamped() const { return statistics().frames_quantiser_clamped; }

    /// A frame in memory the encoder reads directly, when the backend can hand one out.
    Fallible<PictureStorePtr> create_frame(MemoryKind memory);

    // `pts` is already in the encoder time base. `rects` are in pixels, strongest first --
    // on overlap the lowest index wins.
    //
    // Pixels in a form the encoder does not read directly are converted, and uploaded if
    // it wants hardware frames; both are counted. A buffer from create_frame needs neither.
    //
    // `sei_unregistered` is UUID||payload for bitstream metadata; empty skips.
    void encode_buffer(IPictureStore& pixels, const std::vector<Area>& rects, int64_t pts,
        std::span<const uint8_t> sei_unregistered = {});

    // Drains the encoder. Does not end the sink.
    void flush();

private:
    // Pulls packets until the backend says there are none, handing each to the sink. The
    // pull loop is here rather than in the backend because the backend only offers
    // ReceiveAccessUnit and the loop belongs to whoever owns the pipeline -- which, for the
    // file and frame entry points, is this class.
    void drain();

    /// What the session did, asked of the backend that owns it, so that a backend need not
    /// be a libavcodec backend for this to work.
    EngineCounters statistics() const
    {
        return m_backend && m_session ? m_backend->Statistics(*m_session.get())
                                      : EngineCounters{};
    }

    const EngineCatalog* m_registry;
    Engine m_kind = Engine::X264;
    CodingParams m_params;
    long m_backends_declined = 0;
    std::vector<std::string> m_backends_skipped;
    Coding m_codec = Coding::H265;
    /// What the session was opened with -- see capabilities().
    uint64_t m_target_bitrate = 0;
    const IEngine* m_backend = nullptr;
    TranscoderPtr m_session;

    // One frame's worth of the encoder time base, used when the encoder leaves
    // a packet's duration unset.
    int64_t m_frame_duration = 0;
    AccessUnitOutlet* m_sink = nullptr;
};

} // namespace ta_roienc
