#pragma once

// What every backend shares: the parts of Configure that do not depend on which encoder
// opened, and the whole of SubmitPicture / ReceiveAccessUnit / Flush.
//
// The libavcodec-backed backends implement the last three identically -- they are the same
// avcodec_send_frame / avcodec_receive_packet pair, because every one of them reaches the
// hardware through libavcodec. Putting that implementation in a base class rather than in
// each backend keeps one copy of it.

#include "engine/common/engine_run.h"
#include "ta_roienc/engine.h"

#include <span>

namespace ta_roienc {

namespace backend_common {

/// What an encoder will actually take, asked of it rather than assumed: whether libx265 was
/// built with a ten-bit path, and whether the driver exposes one, are properties of the
/// machine, not of a table somebody wrote down.
struct TenBitAbility {
    bool main10 = false;      ///< accepts some ten-bit pixel format
    bool p010_input = false;  ///< accepts AV_PIX_FMT_P010 without conversion
    int encode_format = -1;   ///< AV_PIX_FMT_NONE when main10 is false
};

TenBitAbility query_ten_bit(const char* engine_name);


/// Fills the parts of an AVCodecContext that are the same whoever encodes.
void apply_shared(AVCodecContext* enc, const EngineSetup& p);

/// Opens a hardware device, returning false rather than throwing when it is absent --
/// an absent GPU is the normal case on some machines, not an error.
bool open_hw_device(roiav::ByteBlock& hw, AVHWDeviceType type, const char* device);

/// Allocates the encoder's own pool of hardware surfaces. Owning the pool is what makes a
/// frame factory possible at all: a surface handed to the caller has to come from the pool
/// the encoder will read, or it is just another buffer to copy from.
bool init_hw_frames(roiav::ByteBlock& device, roiav::ByteBlock& frames, AVPixelFormat hw_format,
    AVPixelFormat sw_format, int width, int height, int pool_size);

/// Attaches the regions to a frame as AV_FRAME_DATA_REGIONS_OF_INTEREST. This is the whole
/// adaptive mechanism at the FFmpeg boundary: libavcodec turns it into mfxExtEncoderROI on
/// QSV, VAEncMiscParameterBufferROI on VA-API and quantOffsets on x264/x265.
void attach_roi(AVFrame* frame, std::span<const Area> regions);

/// Attaches user-data-unregistered SEI (UUID || payload) for libx264/libx265 `udu_sei`.
void attach_sei_unregistered(AVFrame* frame, std::span<const uint8_t> uuid_and_payload);

} // namespace backend_common

/// Everything a libavcodec-backed backend shares: opening the codec, submitting frames,
/// draining packets, and handing out frames from the encoder's own memory. A backend
/// inherits this and says only three things -- which encoder it is, what it can do, and
/// what to set on the context once it exists.
class LibavEngine : public IEngine {
public:
    Fallible<TranscoderPtr> Configure(const EngineSetup& config) const override;
    Fallible<PictureStorePtr> CreatePicture(
        EngineRun& session, MemoryKind memory) const override;
    Verdict SubmitPicture(EngineRun& session, IPictureStore& frame,
        const EnginePictureControl& control) const override;
    Fallible<AccessUnit> ReceiveAccessUnit(EngineRun& session) const override;
    Verdict Flush(EngineRun& session) const override;
    Fallible<CodingParams> GetCodingParams(EngineRun& session) const override;
    EngineCounters Statistics(const EngineRun& session) const override;
    /// One override for the whole group: every backend here reaches its device through the
    /// same AVBufferRef, and the software ones simply never opened one.
    NativeHandle HardwareDevice(const EngineRun& session) const override;

protected:
    /// The libavcodec encoder to open, by output codec.
    virtual const char* codec_name(Coding codec) const = 0;

    /// Fills the context and opens whatever device this backend needs. Called with the
    /// context allocated and the shared fields already applied, and before
    /// avcodec_open2 -- which is the only window in which most of these options are
    /// settable at all.
    virtual Verdict configure_context(
        LibavRun& session, const EngineSetup& config) const = 0;
};

} // namespace ta_roienc
