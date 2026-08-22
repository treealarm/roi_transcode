#pragma once

// One encoder backend: how to open it, and what it can do once open.
//
// TZ v2.0 §7 describes IEncoderBackend with five methods -- GetCapabilities, Configure,
// SubmitFrame, ReceivePacket, Flush. Two of those are here. The other three are not, and
// the reason is worth stating rather than hiding: every backend we have submits frames
// through the same libavcodec calls, so splitting SubmitFrame/ReceivePacket per backend
// today would produce three identical implementations and an interface that proves
// nothing. What actually differs between QSV, VA-API, NVENC and libx264 is how the
// context is opened and what the hardware will accept -- which is exactly what this
// interface carries.
//
// It grows the rest when a backend needs it. Rockchip MPP will: it does not go through
// libavcodec at all.

#include "av_raii.h"
#include "smartvideo/capabilities.h"
#include "smartvideo/types.h"

#include <memory>
#include <vector>

namespace smartvideo {

struct EncoderParams {
    int width = 0;
    int height = 0;
    // Frame rate. Also picks the default GOP (two seconds) when gop == 0.
    AVRational fps{25, 1};
    // Encoder time base. Left at den == 0 it follows the frame rate, which is
    // what a file wants; a pushed source usually has its own units instead.
    AVRational time_base{0, 0};
    Encoder kind = Encoder::Auto;
    Codec codec = Codec::H265;
    int crf = 28;
    int gop = 0;
    std::string preset = "medium";
};

/// An encoder that opened, plus whatever the backend needed to open it. Ownership lives
/// here so a failed attempt frees everything by going out of scope.
struct BackendSession {
    Encoder kind = Encoder::Cpu;
    roiav::CodecCtx enc;
    roiav::Buffer hw_device;
    roiav::Buffer hw_frames;
    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    /// The encoder takes hardware frames, so software frames must be uploaded first.
    bool hw_upload = false;
};

class IEncoderBackend {
public:
    virtual ~IEncoderBackend() = default;

    virtual Encoder kind() const = 0;

    /// What this backend can do for a given output codec. Static for the backends here;
    /// a backend that can only answer by asking a driver may query on first use.
    virtual EncoderCapabilities GetCapabilities(Codec codec) const = 0;

    /// Configures an already-allocated context for this backend and opens the device it
    /// needs. False means "not available on this machine" -- the caller moves to the next
    /// backend in the chain, so this must not throw for an absent GPU.
    virtual bool Configure(BackendSession& out, const EncoderParams& params) const = 0;

    /// Some backends open successfully and only fail on the first frame. Those ask for a
    /// probe: the caller encodes one throwaway frame, and reopens if it worked.
    virtual bool NeedsProbe() const { return false; }
};

/// The backends to try, in order, for a requested kind. Encoder::Auto returns the whole
/// fallback chain; a named kind returns just that one, so asking for QSV and getting
/// libx264 is not something that can happen silently.
std::vector<const IEncoderBackend*> backend_chain(Encoder requested);

/// The backend that opened, by kind -- for capability queries after the fact.
const IEncoderBackend* backend_for(Encoder kind);

} // namespace smartvideo
