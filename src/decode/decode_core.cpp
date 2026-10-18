#include "decode_hardware_access.h"

#include "engine/common/av_picture_store.h"
#include "picture_input.h"
#include "ta_roienc/picture.h"

#include <exception>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace ta_roienc {

struct VideoDecoder::Internals {
    PictureDecoder decoder;
    /// Held by value so the device cannot outlive its use here. It is a shared handle, so
    /// this is a reference count and not a copy of the device.
    DecodeDevice device;
    DecodeOutcome report;
    bool opened = false;
};

namespace {

/// The JPEG-range twins, and the limited-range format each of them is really laid out as.
///
/// libavcodec still reports yuvj420p for MJPEG and for the H.264 cameras that flag full
/// range, and PixelLayout has no entry for it -- deliberately, since the range is a
/// property of the samples rather than of the layout. So the layout is normalised here and
/// the range travels beside it in DecodedPictureInfo, which is the only place it can be read
/// without guessing.
AVPixelFormat limited_range_twin(int format, bool& full_range) noexcept
{
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
        full_range = true;
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        full_range = true;
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        full_range = true;
        return AV_PIX_FMT_YUV444P;
    default:
        return static_cast<AVPixelFormat>(format);
    }
}

DecodedPictureInfo describe(AVFrame* frame, Stamp timestamp)
{
    DecodedPictureInfo info;
    info.timestamp = timestamp;
    info.key_frame = (frame->flags & AV_FRAME_FLAG_KEY) != 0;

    bool full_range = false;
    frame->format = limited_range_twin(frame->format, full_range);
    // A stream may also say so through color_range without using a yuvj format at all.
    info.full_range = full_range || frame->color_range == AVCOL_RANGE_JPEG;
    return info;
}

} // namespace

VideoDecoder::VideoDecoder() : m_impl(std::make_unique<Internals>()) {}
VideoDecoder::~VideoDecoder() = default;

Verdict VideoDecoder::Opened(
    SourceCodec codec, std::span<const uint8_t> extradata, DecodeDevice device)
{
    if (m_impl->opened) {
        return Verdict::InvalidArgument("this decoder is already open");
    }
    m_impl->device = std::move(device);
    try {
        m_impl->decoder.open(codec, extradata.data(), static_cast<int>(extradata.size()),
            DecodeDeviceAccess::device(m_impl->device),
            PictureDecoder::Framing::CompleteAccessUnits);
    } catch (const std::exception& e) {
        return Verdict::BackendError(e.what());
    }
    m_impl->opened = true;
    return Verdict::Ok();
}

Verdict VideoDecoder::Push(
    std::span<const uint8_t> access_unit, Stamp timestamp, const PictureCb& on_frame)
{
    if (!m_impl->opened) {
        return Verdict::InvalidArgument("Push before Open");
    }
    try {
        m_impl->decoder.push(access_unit.data(), access_unit.size(), timestamp.value,
            [&](AVFrame* decoded) {
                const DecodedPictureInfo info = describe(decoded, timestamp);
                ++m_impl->report.frames_decoded;
                if (on_frame) {
                    on_frame(AvPictureBridge::Ref(decoded), info);
                }
            });
    } catch (const std::exception& e) {
        return Verdict::BackendError(e.what());
    }
    return Verdict::Ok();
}

Verdict VideoDecoder::Flush(const PictureCb& on_frame)
{
    if (!m_impl->opened) {
        return Verdict::Ok();
    }
    try {
        m_impl->decoder.flush([&](AVFrame* decoded) {
            // Nothing left to date the frame by: the access unit it belonged to is long
            // gone. An unset timestamp is the honest answer, and Stamp already
            // has one -- inventing a plausible number here is how a timeline goes wrong
            // in a way nobody can trace back.
            const DecodedPictureInfo info = describe(decoded, Stamp{});
            ++m_impl->report.frames_decoded;
            if (on_frame) {
                on_frame(AvPictureBridge::Ref(decoded), info);
            }
        });
    } catch (const std::exception& e) {
        return Verdict::BackendError(e.what());
    }
    return Verdict::Ok();
}

Fallible<Picture> VideoDecoder::Download(const Picture& frame)
{
    if (!frame) {
        return Verdict::InvalidArgument("Download was given no frame");
    }
    if (frame.buffer().format() != PixelLayout::HardwareSurface) {
        // Already readable. Returning it as it stands rather than refusing, so that a
        // caller's code path does not have to fork on where the decode happened to run.
        return frame;
    }

    const auto* source = dynamic_cast<const AvPictureStore*>(&frame.buffer());
    if (source == nullptr || source->frame() == nullptr) {
        return Verdict::InvalidArgument("this frame did not come from a decoder here");
    }

    roiav::Picture downloaded;
    if (downloaded.p == nullptr) {
        return Verdict::OutOfMemory("could not allocate a frame to download into");
    }
    if (av_hwframe_transfer_data(downloaded.p, source->frame(), 0) < 0) {
        ++m_impl->report.download_failures;
        return Verdict::BackendError("the frame could not be brought back from the device");
    }
    // The transfer moves pixels and nothing else -- timestamps, the keyframe flag and the
    // colour properties all stay behind unless they are copied over deliberately.
    av_frame_copy_props(downloaded.p, source->frame());

    AVFrame* raw = downloaded.p;
    downloaded.p = nullptr;
    ++m_impl->report.frames_downloaded;
    return AvPictureBridge::Adopt(raw);
}

std::string_view VideoDecoder::decoder_name() const noexcept
{
    return m_impl->decoder.decoder_name();
}

std::string_view VideoDecoder::hardware_name() const noexcept
{
    return m_impl->decoder.hardware_name();
}

DecodeOutcome VideoDecoder::report() const noexcept
{
    return m_impl->report;
}

} // namespace ta_roienc
