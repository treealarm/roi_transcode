#pragma once

// What every backend shares: the parts of Configure that do not depend on which encoder
// opened, and the whole of SubmitFrame / ReceivePacket / Flush.
//
// §7 gives all five methods to IEncoderBackend, and all four of our backends implement the
// last three identically -- they are the same avcodec_send_frame / avcodec_receive_packet
// pair, because every one of them reaches the hardware through libavcodec. Putting that
// implementation in a base class rather than in each backend is what keeps the interface
// faithful to the specification without three copies of one function behind it. A backend
// that does not go through libavcodec at all (Rockchip MPP, §6) overrides them.

#include "backend/common/backend_session.h"
#include "smartvideo/encoder_backend.h"

#include <span>

namespace smartvideo {

namespace backend_common {

/// Fills the parts of an AVCodecContext that are the same whoever encodes.
void apply_shared(AVCodecContext* enc, const EncoderConfig& p);

/// Opens a hardware device, returning false rather than throwing when it is absent --
/// an absent GPU is the normal case on some machines, not an error.
bool open_hw_device(roiav::Buffer& hw, AVHWDeviceType type, const char* device);

/// Allocates the encoder's own pool of hardware surfaces. Owning the pool is what makes a
/// frame factory possible at all: a surface handed to the caller has to come from the pool
/// the encoder will read, or it is just another buffer to copy from.
bool init_hw_frames(roiav::Buffer& device, roiav::Buffer& frames, AVPixelFormat hw_format,
    AVPixelFormat sw_format, int width, int height, int pool_size);

/// Attaches the regions to a frame as AV_FRAME_DATA_REGIONS_OF_INTEREST. This is the whole
/// adaptive mechanism at the FFmpeg boundary: libavcodec turns it into mfxExtEncoderROI on
/// QSV, VAEncMiscParameterBufferROI on VA-API and quantOffsets on x264/x265.
void attach_roi(AVFrame* frame, std::span<const Rect> regions);

} // namespace backend_common

/// Everything a libavcodec-backed backend shares: opening the codec, submitting frames,
/// draining packets, and handing out frames from the encoder's own memory. A backend
/// inherits this and says only three things -- which encoder it is, what it can do, and
/// what to set on the context once it exists.
class LibavBackend : public IEncoderBackend {
public:
    Result<SessionPtr> Configure(const EncoderConfig& config) const override;
    Result<FrameBufferPtr> CreateFrame(
        BackendSession& session, MemoryType memory) const override;
    Status SubmitFrame(BackendSession& session, IFrameBuffer& frame,
        const BackendFrameControl& control) const override;
    Result<EncodedPacket> ReceivePacket(BackendSession& session) const override;
    Status Flush(BackendSession& session) const override;

protected:
    /// The libavcodec encoder to open, by output codec.
    virtual const char* codec_name(Codec codec) const = 0;

    /// Fills the context and opens whatever device this backend needs. Called with the
    /// context allocated and the shared fields already applied, and before
    /// avcodec_open2 -- which is the only window in which most of these options are
    /// settable at all.
    virtual Status configure_context(
        LibavSession& session, const EncoderConfig& config) const = 0;
};

} // namespace smartvideo
