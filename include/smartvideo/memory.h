#pragma once

// Where a frame's pixels live -- TZ v2.0 §8.
//
// All seven are named because the enum is part of the contract: a caller compiled against
// this header keeps compiling when a backend learns a new one. Only the two the encoder
// can take today are accepted; the rest are refused at submit with a message saying so,
// which is a better answer than a wrong picture.

namespace smartvideo {

enum class MemoryType {
    /// Tightly packed planes in system memory. Accepted.
    CPU,
    /// An AVFrame the caller owns, hardware or software. Accepted.
    AVFRAME,
    /// CUDA device memory. Needs the NVENC backend to be finished (§4).
    CUDA,
    /// VASurfaceID. The VA-API backend uploads software frames itself today.
    VAAPI,
    /// mfxFrameSurface1. The oneVPL backend uploads software frames itself today.
    ONEVPL,
    /// Rockchip MPP buffer. Needs the MPP backend (§6).
    ROCKCHIP_MPP,
    /// A dma-buf file descriptor, shared with the encoder without a copy.
    DMABUF,
};

/// Whether the encoder can take pixels in this form at all. False is not a defect: it is
/// the honest answer for a backend that does not exist yet, and the caller learns it
/// before submitting a frame rather than after.
constexpr bool memory_type_supported(MemoryType t)
{
    return t == MemoryType::CPU || t == MemoryType::AVFRAME;
}

const char* memory_type_name(MemoryType t);

} // namespace smartvideo
