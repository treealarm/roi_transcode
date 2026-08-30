// Frame ownership and raw-plane wrapping -- TZ v3.4 §8.5 and §8.10.
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
    CHECK(frame.get()->width == kWidth);
    CHECK(frame.get()->height == kHeight);
    CHECK(frame.get()->pts == 1000);
    // No copy: the frame points at the caller's own bytes.
    CHECK(frame.get()->data[0] == pixels->y.data());
    CHECK(frame.get()->linesize[0] == kWidth);
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
    CHECK(frame.get()->data[0] == borrowed);
    CHECK(frame.get()->data[0][0] == 42);
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
    auto pixels = std::make_shared<Pixels>();
    Frame wrapped;
    CHECK(WrapRawFrame(view_of(pixels), &wrapped).ok());

    Frame second = Frame::Ref(wrapped.get());
    CHECK(static_cast<bool>(second));
    CHECK(second.get() != wrapped.get());
    // Same pixels behind both, and destroying either leaves the other usable.
    CHECK(second.get()->data[0] == wrapped.get()->data[0]);
    CHECK(second.get()->width == kWidth);
}

void an_empty_frame_is_a_normal_state()
{
    Frame empty;
    CHECK(!empty);
    CHECK(empty.get() == nullptr);
    CHECK(!Frame::Ref(nullptr));
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
    an_empty_frame_is_a_normal_state();
    std::printf("ok\n");
    return 0;
}
