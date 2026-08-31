// Frame ownership and raw-plane wrapping -- TZ v4.1 §8.5 and §8.10.
//
// Asserted through IFrameBuffer rather than through the AVFrame behind it. Since 4.1
// that is the interface a caller actually holds, and a test reaching past it would be
// checking an implementation detail this header exists to hide.
//
// What is worth testing here is not that a wrap succeeds; it is every way it must refuse.
// A view that lies about its plane lengths is the failure this function exists to catch,
// and catching it here rather than inside an encoder is the whole difference between an
// InvalidArgument and a read past the end of somebody's buffer.

#include "sve/frame.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace sve;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 32;

struct Pixels {
    std::vector<uint8_t> y = std::vector<uint8_t>(kWidth * kHeight, 128);
    std::vector<uint8_t> u = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
    std::vector<uint8_t> v = std::vector<uint8_t>(kWidth * kHeight / 4, 128);
};

/// A view over pixels held by a shared_ptr, which is what a caller with their own pipeline
/// actually has. `owner` keeps them alive; the test below leans on that.
RawFrameView view_of(const std::shared_ptr<Pixels>& pixels)
{
    RawFrameView view;
    view.format.pixel_format = PixelFormat::YUV420P;
    view.format.width = kWidth;
    view.format.height = kHeight;
    view.timestamp = MediaTimestamp{1000, Rational{1, 1000}};
    view.plane_count = 3;
    view.planes[0] = {pixels->y.data(), kWidth, pixels->y.size()};
    view.planes[1] = {pixels->u.data(), kWidth / 2, pixels->u.size()};
    view.planes[2] = {pixels->v.data(), kWidth / 2, pixels->v.size()};
    view.owner = pixels;
    return view;
}

void a_described_frame_wraps_without_copying()
{
    auto pixels = std::make_shared<Pixels>();
    Frame frame;
    const Status s = WrapRawFrame(view_of(pixels), &frame);
    CHECK(s.ok());
    CHECK(static_cast<bool>(frame));
    CHECK(frame.buffer().width() == kWidth);
    CHECK(frame.buffer().height() == kHeight);
    CHECK(frame.buffer().format() == PixelFormat::YUV420P);
    // No copy: the buffer points at the caller's own bytes.
    const PlaneView luma = frame.mutable_buffer().plane(0);
    CHECK(luma.bytes.data() == pixels->y.data());
    CHECK(luma.stride == kWidth);
}

void the_owner_outlives_the_caller_dropping_it()
{
    auto pixels = std::make_shared<Pixels>();
    pixels->y[0] = 42;
    Frame frame;
    CHECK(WrapRawFrame(view_of(pixels), &frame).ok());

    const uint8_t* borrowed = pixels->y.data();
    pixels.reset();
    // The frame holds the last reference, so the pixels it points at are still there.
    // Without that, this read is the use-after-free the owner field exists to prevent.
    const PlaneView luma = frame.mutable_buffer().plane(0);
    CHECK(luma.bytes.data() == borrowed);
    CHECK(luma.bytes[0] == 42);
}

void a_view_without_an_owner_is_refused()
{
    auto pixels = std::make_shared<Pixels>();
    RawFrameView view = view_of(pixels);
    view.owner.reset();
    Frame frame;
    const Status s = WrapRawFrame(view, &frame);
    CHECK(s.code() == StatusCode::InvalidArgument);
    CHECK(!frame);
}

void a_short_plane_is_refused()
{
    auto pixels = std::make_shared<Pixels>();
    RawFrameView view = view_of(pixels);
    // One row short of what the stride and the height together claim.
    view.planes[0].size -= kWidth;
    Frame frame;
    CHECK(WrapRawFrame(view, &frame).code() == StatusCode::InvalidArgument);
    CHECK(!frame);
}

void the_wrong_plane_count_is_refused()
{
    auto pixels = std::make_shared<Pixels>();
    RawFrameView view = view_of(pixels);
    // YUV420P has three planes; claiming two is a caller who meant NV12.
    view.plane_count = 2;
    Frame frame;
    CHECK(WrapRawFrame(view, &frame).code() == StatusCode::InvalidArgument);
}

void a_format_with_no_pixels_is_refused()
{
    auto pixels = std::make_shared<Pixels>();
    RawFrameView view = view_of(pixels);
    view.format.pixel_format = PixelFormat::HardwareSurface;
    Frame frame;
    // HardwareSurface names a class of formats rather than one, so there is nothing here
    // to wrap and Unsupported is the honest answer rather than InvalidArgument.
    CHECK(!WrapRawFrame(view, &frame).ok());
}

void an_invalid_format_is_refused()
{
    auto pixels = std::make_shared<Pixels>();
    RawFrameView view = view_of(pixels);
    view.format.height = 0;
    Frame frame;
    CHECK(WrapRawFrame(view, &frame).code() == StatusCode::InvalidArgument);
}

void ref_leaves_the_caller_their_own_frame()
{
    // The interop adapter, on a frame the caller allocated and still owns afterwards.
    AVFrame* theirs = av_frame_alloc();
    CHECK(theirs != nullptr);
    theirs->format = AV_PIX_FMT_YUV420P;
    theirs->width = kWidth;
    theirs->height = kHeight;
    CHECK(av_frame_get_buffer(theirs, 0) == 0);
    theirs->data[0][0] = 77;

    Frame held = AvFrameAdapter::Ref(theirs);
    CHECK(static_cast<bool>(held));
    CHECK(held.buffer().width() == kWidth);
    // Same pixels behind both: a reference, not a copy.
    CHECK(held.mutable_buffer().plane(0).bytes.data() == theirs->data[0]);

    // The caller unrefs theirs and the Frame still reads the picture, which is what Ref
    // promises and what Adopt would not have allowed them to do at all.
    av_frame_free(&theirs);
    CHECK(held.mutable_buffer().plane(0).bytes[0] == 77);
}

void copies_of_a_frame_share_one_buffer()
{
    // §8.5 makes Frame copyable and shared, because an encoder with lookahead holds a
    // frame past the call that submitted it.
    auto pixels = std::make_shared<Pixels>();
    Frame first;
    CHECK(WrapRawFrame(view_of(pixels), &first).ok());
    Frame second = first;
    CHECK(&first.buffer() == &second.buffer());
}

void an_empty_frame_is_a_normal_state()
{
    Frame empty;
    CHECK(!empty);
    CHECK(!AvFrameAdapter::Ref(nullptr));
    CHECK(!AvFrameAdapter::Adopt(nullptr));
}

} // namespace

int main()
{
    a_described_frame_wraps_without_copying();
    the_owner_outlives_the_caller_dropping_it();
    a_view_without_an_owner_is_refused();
    a_short_plane_is_refused();
    the_wrong_plane_count_is_refused();
    a_format_with_no_pixels_is_refused();
    an_invalid_format_is_refused();
    ref_leaves_the_caller_their_own_frame();
    copies_of_a_frame_share_one_buffer();
    an_empty_frame_is_a_normal_state();
    std::printf("ok\n");
    return 0;
}
