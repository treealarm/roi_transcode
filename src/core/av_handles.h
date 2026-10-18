#pragma once

// Ownership wrappers and error helpers shared by the encoder and the file
// pipeline. Header-only: these are a handful of lines each and every one of
// them exists only to make a libav* pointer release itself.

#include "failure.h"

#include <format>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace roiav {

struct DemuxCtx {
    AVFormatContext* p = nullptr;
    DemuxCtx() = default;
    DemuxCtx(const DemuxCtx&) = delete;
    DemuxCtx& operator=(const DemuxCtx&) = delete;
    ~DemuxCtx()
    {
        if (p) {
            avformat_close_input(&p);
        }
    }
};

struct MuxCtx {
    AVFormatContext* p = nullptr;
    MuxCtx() = default;
    MuxCtx(const MuxCtx&) = delete;
    MuxCtx& operator=(const MuxCtx&) = delete;
    ~MuxCtx()
    {
        if (p) {
            if (p->oformat && !(p->oformat->flags & AVFMT_NOFILE) && p->pb) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};

struct CodingCtx {
    AVCodecContext* p = nullptr;
    CodingCtx() = default;
    CodingCtx(CodingCtx&& o) noexcept : p(o.p) { o.p = nullptr; }
    CodingCtx& operator=(CodingCtx&& o) noexcept
    {
        if (this != &o) {
            avcodec_free_context(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~CodingCtx() { avcodec_free_context(&p); }
};

struct Picture {
    AVFrame* p = nullptr;
    Picture() : p(av_frame_alloc()) {}
    Picture(Picture&& o) noexcept : p(o.p) { o.p = nullptr; }
    Picture& operator=(Picture&& o) noexcept
    {
        if (this != &o) {
            av_frame_free(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Picture() { av_frame_free(&p); }
    Picture(const Picture&) = delete;
    Picture& operator=(const Picture&) = delete;
};

struct RawUnit {
    AVPacket* p = nullptr;
    RawUnit() : p(av_packet_alloc()) {}
    RawUnit(RawUnit&& o) noexcept : p(o.p) { o.p = nullptr; }
    RawUnit& operator=(RawUnit&& o) noexcept
    {
        if (this != &o) {
            av_packet_free(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~RawUnit() { av_packet_free(&p); }
    RawUnit(const RawUnit&) = delete;
    RawUnit& operator=(const RawUnit&) = delete;
};

struct Scaler {
    SwsContext* p = nullptr;
    Scaler() = default;
    Scaler(Scaler&& o) noexcept : p(o.p) { o.p = nullptr; }
    Scaler& operator=(Scaler&& o) noexcept
    {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    Scaler(const Scaler&) = delete;
    Scaler& operator=(const Scaler&) = delete;
    void reset()
    {
        if (p) {
            sws_freeContext(p);
            p = nullptr;
        }
    }
    ~Scaler() { reset(); }
};

struct ByteBlock {
    AVBufferRef* p = nullptr;
    ByteBlock() = default;
    ByteBlock(ByteBlock&& o) noexcept : p(o.p) { o.p = nullptr; }
    ByteBlock& operator=(ByteBlock&& o) noexcept
    {
        if (this != &o) {
            av_buffer_unref(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~ByteBlock() { av_buffer_unref(&p); }
};

inline void check(int err, const char* what)
{
    if (err >= 0) {
        return;
    }
    char buf[128];
    av_strerror(err, buf, sizeof(buf));
    fail(std::format("{}: {}", what, buf));
}

} // namespace roiav
