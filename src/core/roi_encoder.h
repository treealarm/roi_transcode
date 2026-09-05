#pragma once

// The encoder with region-of-interest side data, independent of where the frames came
// from. A file pipeline and a caller pushing frames want exactly this and nothing else
// around it.
//
// What lives here is backend selection: walking the fallback chain, honouring a probe, and
// reporting which one opened. The frames themselves go through IEncoderBackend (§7) --
// this class no longer touches libavcodec's send/receive pair itself.

#include "sve/backend_registry.h"
#include "sve/encoder_backend.h"
#include "sve/types.h"

#include <span>
#include <string>
#include <vector>

namespace sve {

class PacketSink;

class RoiEncoder {
public:
    /// Which deployment's backends this encoder may open, defaulting to the process-wide
    /// registry -- so the ordinary caller constructs it with nothing, as before.
    ///
    /// It is a constructor argument rather than a global lookup inside open_deferred
    /// because that lookup was this class's one hidden dependency: everything else it
    /// needs arrives through IEncoderBackend or PacketSink, and the chain did not. A
    /// caller assembling its own BackendRegistry -- which sve/backend_registry.h has
    /// offered since 4.2 -- could not reach this pipeline at all.
    ///
    /// **The registry must outlive the encoder.** It is held by pointer, not copied: a
    /// BackendRegistry holds references to backends that are stateless immortal singletons
    /// (Р7), so copying it would be copying a list of pointers and would say nothing about
    /// lifetime anyway. A temporary binds here without a diagnostic, and today survives
    /// only by accident -- open_deferred is the sole reader, and CreateVideoEncoderIn calls
    /// it inside the full-expression that keeps a temporary alive. The two-step form below
    /// has no such luck.
    explicit RoiEncoder(const BackendRegistry& registry = default_backend_registry())
        : m_registry(&registry)
    {
    }

    // Opens a backend, falling back down the chain when kind is Auto, then
    // calls sink.begin(). Throws if nothing opens.
    void open(const BackendConfig& config, PacketSink& sink);

    // The same in two steps, for a caller that must know which backend opened
    // before the sink is told about it.
    void open_deferred(const BackendConfig& config);
    void attach(PacketSink& sink);

    Backend kind() const { return m_kind; }
    /// Backends that were tried and declined before one opened. Normal on a machine
    /// without the hardware; §22 wants the choice observable rather than inferred.
    long backends_declined() const { return m_backends_declined; }
    /// Backends left out of the chain before anything was opened, one reason each -- a
    /// backend that cannot produce the requested codec (§8.4). Kept separate from the
    /// declined count because these were never tried: no device was touched.
    const std::vector<std::string>& backends_skipped() const { return m_backends_skipped; }
    /// What the backend that actually opened is able to do. Only meaningful after open;
    /// before that there is no backend to ask.
    EncoderCapabilities capabilities() const;
    /// What the opened backend settled on -- §38's calibration reads this. Empty before
    /// open, and empty afterwards from a backend that cannot read its own settings back.
    EffectiveSettings effective_settings() const;
    // Human-readable name of the backend that actually opened, codec included.
    const char* label() const;
    /// What the opened encoder produces -- codec, geometry, time base, extradata. Read
    /// from the backend once at open, because that is the only thing every backend can
    /// answer; there is no AVCodecContext here any more to reach into.
    const CodecParameters& codec_parameters() const { return m_params; }

    /// How many frames needed a pixel-format conversion or an upload to the GPU before the
    /// encoder would take them. Both zero is what zero-copy means, and the only way to
    /// tell a factory frame that avoided the copy from one that merely moved it.
    long frames_converted() const { return statistics().frames_converted; }
    long frames_uploaded() const { return statistics().frames_uploaded; }
    /// Frames whose per-block quantiser map wanted a value outside what the codec has.
    long frames_quantiser_clamped() const { return statistics().frames_quantiser_clamped; }

    /// A frame in memory the encoder reads directly, when the backend can hand one out.
    Result<FrameBufferPtr> create_frame(MemoryType memory);

    // `pts` is already in the encoder time base. `rects` are in pixels, strongest first --
    // on overlap the lowest index wins.
    //
    // Pixels in a form the encoder does not read directly are converted, and uploaded if
    // it wants hardware frames; both are counted. A buffer from create_frame needs neither.
    //
    // `sei_unregistered` is UUID||payload for §19 bitstream metadata; empty skips.
    void encode_buffer(IFrameBuffer& pixels, const std::vector<Rect>& rects, int64_t pts,
        std::span<const uint8_t> sei_unregistered = {});

    // Drains the encoder. Does not end the sink.
    void flush();

private:
    // Pulls packets until the backend says there are none, handing each to the sink. The
    // pull loop is here rather than in the backend because §7 puts ReceivePacket on the
    // backend and leaves the loop to whoever owns the pipeline -- which, for the file and
    // frame entry points, is this class.
    void drain();

    /// What the session did, asked of the backend that owns it. This used to reach into
    /// LibavSession directly, which meant every backend had to be a libavcodec backend --
    /// and the first one that was not crashed here rather than at its own code.
    BackendStatistics statistics() const
    {
        return m_backend && m_session ? m_backend->Statistics(*m_session.get())
                                      : BackendStatistics{};
    }

    const BackendRegistry* m_registry;
    Backend m_kind = Backend::X264;
    CodecParameters m_params;
    long m_backends_declined = 0;
    std::vector<std::string> m_backends_skipped;
    Codec m_codec = Codec::H265;
    /// What the session was opened with -- see capabilities().
    uint64_t m_target_bitrate = 0;
    const IEncoderBackend* m_backend = nullptr;
    SessionPtr m_session;

    // One frame's worth of the encoder time base, used when the encoder leaves
    // a packet's duration unset.
    int64_t m_frame_duration = 0;
    PacketSink* m_sink = nullptr;
};

} // namespace sve
