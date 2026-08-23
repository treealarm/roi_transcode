#pragma once

// Ownership wrappers and error helpers shared by the encoder and the file
// pipeline. Header-only: these are a handful of lines each and every one of
// them exists only to make a libav* pointer release itself.

#include <format>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace roiav {

struct FmtIn {
    AVFormatContext* p = nullptr;
    FmtIn() = default;
    FmtIn(const FmtIn&) = delete;
    FmtIn& operator=(const FmtIn&) = delete;
    ~FmtIn()
    {
        if (p) {
            avformat_close_input(&p);
        }
    }
};

struct FmtOut {
    AVFormatContext* p = nullptr;
    FmtOut() = default;
    FmtOut(const FmtOut&) = delete;
    FmtOut& operator=(const FmtOut&) = delete;
    ~FmtOut()
    {
        if (p) {
            if (p->oformat && !(p->oformat->flags & AVFMT_NOFILE) && p->pb) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};

struct CodecCtx {
    AVCodecContext* p = nullptr;
    CodecCtx() = default;
    CodecCtx(CodecCtx&& o) noexcept : p(o.p) { o.p = nullptr; }
    CodecCtx& operator=(CodecCtx&& o) noexcept
    {
        if (this != &o) {
            avcodec_free_context(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~CodecCtx() { avcodec_free_context(&p); }
};

struct Frame {
    AVFrame* p = nullptr;
    Frame() : p(av_frame_alloc()) {}
    Frame(Frame&& o) noexcept : p(o.p) { o.p = nullptr; }
    Frame& operator=(Frame&& o) noexcept
    {
        if (this != &o) {
            av_frame_free(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Frame() { av_frame_free(&p); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

struct Packet {
    AVPacket* p = nullptr;
    Packet() : p(av_packet_alloc()) {}
    Packet(Packet&& o) noexcept : p(o.p) { o.p = nullptr; }
    Packet& operator=(Packet&& o) noexcept
    {
        if (this != &o) {
            av_packet_free(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Packet() { av_packet_free(&p); }
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
};

struct Sws {
    SwsContext* p = nullptr;
    Sws() = default;
    Sws(Sws&& o) noexcept : p(o.p) { o.p = nullptr; }
    Sws& operator=(Sws&& o) noexcept
    {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    Sws(const Sws&) = delete;
    Sws& operator=(const Sws&) = delete;
    void reset()
    {
        if (p) {
            sws_freeContext(p);
            p = nullptr;
        }
    }
    ~Sws() { reset(); }
};

struct Buffer {
    AVBufferRef* p = nullptr;
    Buffer() = default;
    Buffer(Buffer&& o) noexcept : p(o.p) { o.p = nullptr; }
    Buffer& operator=(Buffer&& o) noexcept
    {
        if (this != &o) {
            av_buffer_unref(&p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Buffer() { av_buffer_unref(&p); }
};

[[noreturn]] inline void fail(const std::string& msg)
{
    throw std::runtime_error(msg);
}

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
