#pragma once

// Compressed frames in, pictures out -- decode on its own, without an encoder behind it.
//
// For a consumer that wants pictures and nothing else -- an analytics worker sampling a
// frame a second -- without opening libavcodec itself or dragging an encoder along. It is
// the same FrameDecoder Session uses underneath, so there is one implementation of hardware
// decode in this repository rather than one per caller.
//
// No FFmpeg type is named here. Frames come out as Frame, over IFrameBuffer, exactly as
// they go into the encoder -- so a caller reaches the pixels through plane() and links no
// libavutil to do it.

#include "sve/decode_backend.h"
#include "sve/frame_buffer.h"
#include "sve/status.h"
#include "sve/timed_frame.h"
#include "sve/timestamp.h"
#include "sve/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace sve {

/// The device the decoding runs on: a copyable, shared handle that is always valid.
///
/// Copyable and shared because one device serves every stream in a process while decoders
/// come and go per stream. That is the only shape in which a caller holds neither a raw
/// pointer nor a rule saying the device must outlive the decoders -- Frame in this library
/// is built the same way, and for the same reason.
///
/// Always valid because DecodeBackend::Cpu is a device like any other, not the absence of
/// one. There is no empty state to check for and no null to remember to handle: "decode in
/// software" is a value you can pass. A CPU device holds nothing at all, so it costs no
/// allocation -- the null pointer inside *is* the CPU, and it is not observable as
/// emptiness from the outside.
class DecodeDevice {
public:
    /// The CPU. Always available, holds nothing, and the default.
    DecodeDevice() noexcept = default;
    static DecodeDevice Cpu() noexcept { return {}; }

    /// Opens the first backend in `selection.preferred` that has a device on this machine.
    /// An empty list is InvalidArgument. Backends this build of FFmpeg does not contain,
    /// and backends whose device will not open, are recorded in skipped() and passed over.
    static Result<DecodeDevice> Open(const DecodeSelection& selection);

    DecodeBackend kind() const noexcept;
    /// "vaapi", "qsv", "nvdec", "cpu" -- decode_backend_name of kind().
    std::string_view name() const noexcept;

    /// What was passed over on the way here, one line of reason each. The encoder side
    /// keeps the same record in RoiEncoder::backends_skipped(), so that the choice is
    /// observable rather than inferred from which one ended up running.
    ///
    /// Borrowed from the device: valid while this handle, or any copy of it, is alive.
    std::span<const std::string> skipped() const noexcept;

private:
    struct Impl;
    friend struct DecodeDeviceAccess;
    /// Null is the CPU. See the class comment: this is a state, not an absence.
    std::shared_ptr<const Impl> m_impl;
};

/// What is known about a frame that came out, as opposed to what an encoder is told about
/// one going in. Separate from FrameMetadata for that reason: that struct carries regions
/// and keyframe *requests*, which are instructions, and this one carries observations.
struct DecodedFrameInfo {
    /// The timestamp the access unit was pushed with, in the units it was pushed in.
    MediaTimestamp timestamp;
    bool key_frame = false;
    /// Full-range luma -- yuvj420p and its siblings. Carried explicitly because
    /// PixelFormat has no separate entry for it: the range is a property of the samples,
    /// not of the layout, and a converter that assumes limited range on full-range pixels
    /// produces a visibly flatter picture with nothing to say it went wrong.
    bool full_range = false;
};

/// What a decoder did. Counted rather than described, so "is this actually on the GPU"
/// has an answer that is not a guess -- the same reason EncoderStatistics counts uploads.
struct DecodeReport {
    long frames_decoded = 0;
    /// Frames brought back to system memory by Download. Every one is a copy across the
    /// bus; a caller sampling one frame a second should see one a second here, not the
    /// stream's full rate.
    long frames_downloaded = 0;
    /// Transfers that failed. Non-zero with a hardware decoder means the surface pool is
    /// too small or the device is wedged -- it is not a codec problem, and it is why
    /// hardware decode can need abandoning after it started working.
    long download_failures = 0;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// `extradata` is the codec configuration for AVCC/HVCC input, and may be empty for
    /// Annex-B.
    ///
    /// The device is taken by value: it is a cheap shared handle, so the decoder keeps its
    /// own reference and cannot be left pointing at a device someone else destroyed.
    ///
    /// A hardware device gives frames whose format() is HardwareSurface. A device this
    /// stream cannot use -- a profile the silicon does not implement, most often -- is not
    /// an error: the decode falls back to software, and hardware_name() is empty
    /// afterwards to say that it did. Nothing about that is silent; the caller can see it.
    Status Open(InputCodec codec, std::span<const uint8_t> extradata,
        DecodeDevice device = DecodeDevice::Cpu());

    using FrameCb = std::function<void(const Frame&, const DecodedFrameInfo&)>;

    /// Feeds one access unit -- one complete coded picture, which is what a demuxer hands
    /// out and what the parameter is named for. Calling `on_frame` for every picture that
    /// comes out: usually one, sometimes none while the decoder builds up state, sometimes
    /// several.
    ///
    /// Whole, not a slice of one and not several at once. The decoder takes the promise
    /// literally and does no reassembly, which is what keeps a picture from being held back
    /// until the next one arrives -- a delay of one access unit, and of a whole group of
    /// pictures for a caller that pushes only keyframes.
    Status Push(std::span<const uint8_t> access_unit, MediaTimestamp timestamp,
        const FrameCb& on_frame);
    /// Drains the pictures the decoder is still holding.
    Status Flush(const FrameCb& on_frame);

    /// Brings a frame into system memory so its planes can be read.
    ///
    /// Explicit, and that is the whole economy of decoding on hardware: a caller that
    /// samples one frame a second decodes every frame on the GPU and pays the transfer for
    /// the one it actually looks at. Downloading on the way out of the decoder would spend
    /// the saving before the caller ever saw it.
    ///
    /// A frame already in system memory is returned as it is, with nothing copied.
    Result<Frame> Download(const Frame& frame);

    /// Which libavcodec decoder opened -- "hevc", "h264", "hevc_qsv". Empty before Open.
    std::string_view decoder_name() const noexcept;
    /// The hardware the frames actually came back on -- "vaapi", "qsv" -- and empty when
    /// the decode ran on the CPU.
    ///
    /// Read from the negotiation rather than from what was asked for. libavcodec settles
    /// the pixel format on the first packet, so before then there is an intention and
    /// nothing more, and reporting the intention is how a fallback goes unnoticed.
    std::string_view hardware_name() const noexcept;

    DecodeReport report() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sve
